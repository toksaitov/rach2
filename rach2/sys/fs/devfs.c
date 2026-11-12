/*
 * devfs.c: the devices of /dev, as devfs.h describes them: their file
 * operations, the table that names them, the raw windows onto the disks,
 * and the claims and the writer reservation that keep two users off one
 * range of sectors.
 */

#include "../klog.h"
#include "devfs.h"
#include "procfs.h"
#include "synthfs.h"

#include "../block/loop.h"
#include "fd.h"
#include "file.h"
#include "../proc/process.h"
#include "../mm/usercopy.h"
#include "../lib/kstring.h"
#include "../lib/krandom.h"
#include "../block/mbr.h"
#include "../lib/kerrno.h"
#include "../mm/paging.h"
#include "../mm/kheap.h"
#include "../mm/vm.h"
#include "../io/poll.h"
#include "../tty/pty.h"
#include "../block/block.h"
#include "../block/gpt.h"
#include "../block/usbdisk.h"
#include "../block/recovery.h"
#include "../block/disk.h"
#include "../serial.h"
#include "../rtc.h"
#include "../lib/kirq.h"
#include "../lib/spinlock.h"
#include "../irq.h"
#include "../proc/suspend.h"
#include "../proc/wait_queue.h"
#include "../evdev.h"
#include "../rfkill.h"
#include "../audio/dsp.h"
#include "../audio/mixer.h"
#include "../network/wireless/wsim.h"

/* The open flags `devfs_open` acts on, at Linux's values. */
#define DEVFS_O_CLOEXEC   02000000
#define DEVFS_O_NONBLOCK  04000
#define DEVFS_O_CREAT     0100
#define DEVFS_O_EXCL      0200
#define DEVFS_O_DIRECTORY 0200000

/*
 * The memory devices. /dev/null, /dev/zero and the random devices take a
 * write whole and discard it, and /dev/full fails one with -ENOSPC. A read
 * of zeros or random bytes is copied out of a buffer of 256 bytes on the
 * stack, a piece at a time, so a large count takes no more stack.
 */

/** read of /dev/null and of the devices with nothing to read: the end of
 * the file. */
static int32_t dev_eof_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return 0;
}

/** write that takes the whole buffer and discards it. */
static int32_t dev_sink_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    return (int32_t)count;
}

/** Copy `count` zero bytes, or random ones when `random`, to `buf`; the
 * bytes copied, or -EFAULT when none were. */
static int32_t dev_fill_read(fd_entry_t *e, char __user *buf, uint32_t count, int random)
{
    char tmp[256];
    uint32_t done = 0;

    (void)e;
    if (!random) {
        k_memset(tmp, 0, count < sizeof(tmp) ? count : sizeof(tmp));
    }
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > sizeof(tmp)) {
            chunk = (uint32_t)sizeof(tmp);
        }
        if (random) {
            krandom_bytes(tmp, chunk);
        }
        if (copy_to_user((void __user *)(buf + done), tmp, chunk) != 0) {
            return done > 0 ? (int32_t)done : -EFAULT;
        }
        done += chunk;
    }
    return (int32_t)count;
}

/** read of /dev/zero and /dev/full: zero bytes. */
static int32_t dev_zero_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return dev_fill_read(e, buf, count, 0);
}

/**
 * Read /dev/random or /dev/urandom, failing closed: -EIO when the CSPRNG was
 * seeded only from boot jitter, so no weak randomness is handed out for a
 * key. With RDSEED/RDRAND present the source is trusted and reads succeed.
 */
static int32_t dev_rand_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    if (!krandom_is_trusted()) {
        return -EIO;
    }
    return dev_fill_read(e, buf, count, 1);
}

/** write of /dev/full: -ENOSPC, as on Linux. */
static int32_t dev_full_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -ENOSPC;
}

/** poll of a device that is never waited for: readable and writable at
 * once, with no wait queue, though a transfer of a raw window may still
 * sleep in its disk request. */
static short dev_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    (void)e;
    if (wq) {
        *wq = NULL;
    }
    return (short)(events & (POLLIN | POLLOUT));
}

static int32_t dev_disk_rescan(fd_entry_t *e);
static int32_t dev_getgeo(fd_entry_t *e, uintptr_t arg);
static int32_t dev_sector_size(fd_entry_t *e, uintptr_t arg);
static int32_t dev_block_info(fd_entry_t *e, uint32_t request, uintptr_t arg);

/**
 * The ioctls of a device: the block device requests, which a device with no
 * size (`devfs_fd_raw_size`) answers with -ENOTTY; every other request is
 * -ENOTTY. A checker asks a block device its size before it reads it:
 * `BLKGETSIZE64` is `_IOR(0x12, 114, size_t)`, which is 0x80041272 with
 * musl's 32-bit `size_t` and 0x80081272 from a 64-bit header, and both take
 * eight bytes; `BLKGETSIZE` (0x1260) is the sector count in an unsigned
 * long, -EFBIG when it does not fit. `BLKSSZGET` (0x1268) and `BLKPBSZGET`
 * (0x127B) give the logical and the physical sector size, an int each,
 * which BusyBox's `mkfs.vfat` reads before it lays out a volume.
 * `BLKRRPART` (0x125F) rereads a whole disk's partition table, and
 * `HDIO_GETGEO` (0x0301) gives a geometry and the node's first sector
 * (`dev_getgeo`). `BLKROGET` (0x125E), `BLKRAGET` (0x1263) and `BLKBSZGET`
 * (`_IOR(0x12, 112, size_t)`) give whether the device is read-only, its
 * read-ahead in sectors and its block size, which util-linux's blockdev
 * reads for every device it reports.
 */
static int32_t dev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    uint64_t size;

    if (request == 0x125Eu || request == 0x1263u || request == 0x80041270u ||
        request == 0x80081270u) {
        return dev_block_info(e, request, arg);
    }

    if (request == 0x80041272u || request == 0x80081272u) {
        if (!devfs_fd_raw_size(e, &size)) {
            return -ENOTTY;
        }
        return copy_to_user((void __user *)arg, &size, sizeof(size)) != 0 ? -EFAULT : 0;
    }
    if (request == 0x1268u || request == 0x127Bu) {
        return dev_sector_size(e, arg);
    }
    if (request == 0x125Fu) {
        return dev_disk_rescan(e);
    }
    if (request == 0x0301u) {
        return dev_getgeo(e, arg);
    }
    if (request == 0x1260u) {
        unsigned long sectors;

        if (!devfs_fd_raw_size(e, &size)) {
            return -ENOTTY;
        }
        sectors = (unsigned long)(size >> 9);
        if (sectors != size >> 9) {
            return -EFBIG;
        }
        return copy_to_user((void __user *)arg, &sectors, sizeof(sectors)) != 0 ? -EFAULT : 0;
    }
    return -ENOTTY;
}

/*
 * A device of the table: its name under /dev, the major and minor numbers
 * stat reports for it, and the operations an open binds to a descriptor;
 * the table is below.
 */
struct devfs_entry {
    const char *name;
    uint16_t major;
    uint16_t minor;
    uint8_t lseek_zero; /* 1: an lseek returns 0 (`devfs_lseek_zero`) */
    uint16_t mode;      /* the nine permission bits, unless `devfs_setattr` gave others */
    const struct file_ops *ops;
};

/*
 * The static staging buffer, and the number of sectors one driver call has.
 *
 * A user pointer never goes to a driver: a controller is given the physical
 * address of a transfer's buffer, so the bytes must be where the kernel
 * knows that address. This array is in the kernel image's .bss, below
 * HARD_MEMORY_LIMIT, where its pages are contiguous and `virt_to_phys` gives
 * the address.
 *
 * One driver call moves up to RAW_BATCH_SECTORS, a mebibyte, and fewer on a
 * device whose requests are smaller (`block_max_sectors`). A batch that
 * large keeps a whole-disk reflash short: one sector per call would make a
 * 500 MiB write about a million driver calls, and as many cache flushes on
 * a SATA disk, whose drivers flush the drive's cache after every write.
 *
 * A transfer stages through pages of its own from the heap, so two of them
 * on two disks each sleep on their own device; this buffer, of
 * DEVFS_RAW_BATCH_STATIC_SECTORS, is the one taken in turn when the heap has
 * none or RAW_BATCH_POOL batches are out. Interrupts do still run, so no
 * interrupt path may touch it.
 */
#define RAW_BATCH_SECTORS DEVFS_RAW_BATCH_SECTORS
static uint8_t raw_batch[DEVFS_RAW_BATCH_STATIC_SECTORS * BLOCK_SECTOR_SIZE]
    __attribute__((aligned(4096)));

/* The driver calls the raw windows have made (`devfs_raw_calls`). */
static uint64_t raw_calls;

/*
 * The pid of the process that has the static staging buffer, or -1 while it
 * is free. A window over a device that sleeps holds its buffer across the
 * sleep, and a second process filling the same buffer under the sleeper
 * would hand it the wrong sectors; so the static buffer is taken first and
 * given back after the copy, and a second process that also found the heap
 * empty sleeps until then.
 */
static int raw_batch_holder = -1;
static wait_queue_t raw_batch_wq;

/** The wait condition of `raw_batch_take`: the static buffer is free. */
static int raw_batch_free(void *arg)
{
    (void)arg;
    return raw_batch_holder == -1;
}

/* How many heap batches may be out at once, 8 MiB at the most; a caller
 * past that takes the static buffer in turn, so that callers waiting for a
 * request do not hold the heap in staging. */
#define RAW_BATCH_POOL 8
static uint32_t raw_batches_out;

/**
 * A staging buffer for one transfer of `want` sectors to `device`: the
 * least of the call, the device's largest request and the batch's limit,
 * from the heap while fewer than RAW_BATCH_POOL are out; else the static
 * buffer, taken in turn. `*sectors` receives the buffer's size, which
 * `raw_batch_put` is given back. Waits for the static buffer while another
 * process has it, and a signal does not end the wait.
 */
static uint8_t *raw_batch_take(uint32_t *sectors, uint32_t want, block_device_t *device)
{
    uint32_t most = block_max_sectors(device);
    uint32_t n = want < most ? want : most;
    uint8_t *batch = 0;

    if (n > RAW_BATCH_SECTORS) {
        n = RAW_BATCH_SECTORS;
    }
    if (n == 0) {
        n = 1;
    }
    if (raw_batches_out < RAW_BATCH_POOL) {
        batch = kalloc_pages_try((n * BLOCK_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE, 0);
    }
    if (batch) {
        raw_batches_out++;
        *sectors = n;
        return batch;
    }
    while (raw_batch_holder != -1) {
        wq_wait_event_nointr(&raw_batch_wq, raw_batch_free, 0);
    }
    raw_batch_holder = current_proc ? (int)current_proc->pid : 0;
    *sectors = DEVFS_RAW_BATCH_STATIC_SECTORS;
    return raw_batch;
}

/** Give back the staging buffer `batch` of `sectors` sectors that
 * `raw_batch_take` gave, and wake the waiters for the static one. */
static void raw_batch_put(uint8_t *batch, uint32_t sectors)
{
    if (batch != raw_batch) {
        kfree_pages(batch, (sectors * BLOCK_SECTOR_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
        raw_batches_out--;
        return;
    }
    raw_batch_holder = -1;
    wq_wake_all(&raw_batch_wq);
}

uint64_t devfs_raw_calls(void)
{
    return raw_calls;
}

/*
 * /dev/bootpart: the GRUB disk's FAT boot partition as a bounded raw window,
 * published at boot when the boot disk's partition table qualifies its
 * first entry (`devfs_bootpart_publish`), and absent otherwise. Its
 * transfers are whole 512-byte sectors at offsets of whole sectors, staged
 * through a kernel buffer. A write is on the medium when it returns from an
 * ATA, AHCI or NVMe disk: the first two flush the drive's cache after each
 * write, and the NVMe driver writes with FUA. A USB stick may keep it in its
 * cache until a flush.
 */
static struct disk_window bootpart;

/*
 * /dev/grubcore: the sectors of GRUB's BIOS core, as a bounded raw window, so
 * that updating the boot loader has a device; /dev/bootpart cannot be it,
 * since that one is relative to partition 1 and the core is not in it. On an
 * MBR disk the window is the post-MBR gap, LBA 1 through 2047, a constant of
 * the layout instead of anything the partition table says, so a damaged table
 * cannot widen it. On a GPT disk those sectors are the table's, and the window
 * is the BIOS boot partition, as the checked table gives it. Sector 0 is
 * outside either: it has the partition table, and a disk writes whole sectors.
 */
static struct disk_window grubcore;

/**
 * Read whole sectors from a bounded raw window at the description's
 * `raw_offset`, and move it past them. Returns the bytes read, 0 at or past
 * the end, -ENXIO while the window is not published, -EINVAL for a part of
 * a sector, or -EFAULT or the request's error when nothing was read.
 *
 * Offsets and counts must be whole sectors: a partial sector has no meaning to
 * a disk, and rounding one would either read a neighbour's bytes or write
 * them. The end is handled the way a regular file's is, with a short read,
 * not an error, because a reader walking a window to its end is ordinary.
 * Sectors are staged through a kernel buffer, since a driver is never given
 * a user pointer.
 */
static int32_t raw_window_read(struct disk_window *w, fd_entry_t *e, char __user *buf,
                               uint32_t count)
{
    block_device_t *part = &w->part.device;
    uint64_t off = e->ofd->raw_offset;
    uint64_t total = part->sector_count;
    uint64_t lba;
    uint32_t want, done;
    uint8_t *batch;
    uint32_t batch_sectors;
    int rc = 0;

    if (!w->present) {
        return -ENXIO;
    }
    if ((off % BLOCK_SECTOR_SIZE) != 0 || (count % BLOCK_SECTOR_SIZE) != 0) {
        return -EINVAL;
    }
    lba = off / BLOCK_SECTOR_SIZE;
    if (lba >= total) {
        return 0;
    }
    want = count / BLOCK_SECTOR_SIZE;
    if ((uint64_t)want > total - lba) {
        want = (uint32_t)(total - lba);
    }
    done = 0;
    batch = raw_batch_take(&batch_sectors, want, part);
    while (done < want) {
        uint32_t chunk = want - done;

        if (chunk > batch_sectors) {
            chunk = batch_sectors;
        }
        raw_calls++;
        rc = block_request(part, 0, lba + done, chunk, batch);
        if (rc != 0) {
            break;
        }
        if (copy_to_user((void __user *)(buf + done * BLOCK_SECTOR_SIZE), batch,
                         chunk * BLOCK_SECTOR_SIZE) != 0) {
            if (done == 0) {
                raw_batch_put(batch, batch_sectors);
                return -EFAULT;
            }
            break;
        }
        done += chunk;
    }
    raw_batch_put(batch, batch_sectors);
    if (done == 0) {
        return rc;
    }
    e->ofd->raw_offset = off + (uint64_t)done * BLOCK_SECTOR_SIZE;
    return (int32_t)(done * BLOCK_SECTOR_SIZE);
}

/**
 * Write whole sectors to a bounded raw window at the description's
 * `raw_offset`, and move it past them. Returns the bytes written, -ENXIO
 * while the window is not published, -EPERM for a caller that is not
 * privileged, -EINVAL for a part of a sector, -ENOSPC for a write that
 * starts at or crosses the end, or -EFAULT or -EIO when nothing was written.
 *
 * The whole request is judged before the first sector goes down: a write that
 * starts at or crosses the end writes nothing, because these windows are fixed
 * in size and a half-written boot loader or kernel slot helps nobody. The range
 * arithmetic subtracts instead of adding, so a count near the 32-bit edge
 * cannot wrap past the check.
 *
 * A short return divides the request into three parts, and only the first is a
 * guarantee. The sectors it counts are written. The batch that failed begins at
 * the returned offset and its state is unknown: the block interface answers
 * with zero or an errno and cannot report a partial command, while the ATA
 * driver writes sector by sector inside one and can fail after several have
 * gone. That batch is at most `RAW_BATCH_SECTORS` sectors, and fewer when the
 * device's own request limit is lower, when the static buffer was taken
 * because the heap or the pool was out, or when it was the last batch the
 * request had room for. Everything past it was never attempted. A caller
 * continues by writing again from the returned offset, which covers the
 * unknown batch as well as the rest. Narrowing that unknown range would take
 * a block interface that can say how much of a command completed, which this
 * one cannot.
 *
 * The effective uid is checked here, not trusted from the open. The
 * mode stops a non-root caller at open time and nothing else: the generic
 * credential check returns success the moment the uid is zero, so a process
 * can open this device while privileged, drop privilege, and keep the
 * descriptor. Checking at write time is what makes that useless.
 */
static int32_t raw_window_write(struct disk_window *w, fd_entry_t *e, const char __user *buf,
                                uint32_t count)
{
    block_device_t *part = &w->part.device;
    uint64_t off = e->ofd->raw_offset;
    uint64_t total = part->sector_count;
    uint64_t lba;
    uint32_t want, done;
    uint8_t *batch;
    uint32_t batch_sectors;

    if (!w->present) {
        return -ENXIO;
    }
    if (!current_proc || !proc_privileged(current_proc)) {
        return -EPERM;
    }
    if ((off % BLOCK_SECTOR_SIZE) != 0 || (count % BLOCK_SECTOR_SIZE) != 0) {
        return -EINVAL;
    }
    if (count == 0) {
        return 0;
    }
    lba = off / BLOCK_SECTOR_SIZE;
    want = count / BLOCK_SECTOR_SIZE;
    if (lba >= total || (uint64_t)want > total - lba) {
        return -ENOSPC;
    }
    done = 0;
    batch = raw_batch_take(&batch_sectors, want, part);
    while (done < want) {
        uint32_t chunk = want - done;

        if (chunk > batch_sectors) {
            chunk = batch_sectors;
        }
        if (copy_from_user(batch, (const void __user *)(buf + done * BLOCK_SECTOR_SIZE),
                           chunk * BLOCK_SECTOR_SIZE) != 0) {
            if (done == 0) {
                raw_batch_put(batch, batch_sectors);
                return -EFAULT;
            }
            break;
        }
        raw_calls++;
        if (block_request(part, 1, lba + done, chunk, batch) != 0) {
            break;
        }
        done += chunk;
    }
    raw_batch_put(batch, batch_sectors);
    if (done == 0) {
        return -EIO;
    }
    e->ofd->raw_offset = off + (uint64_t)done * BLOCK_SECTOR_SIZE;
    return (int32_t)(done * BLOCK_SECTOR_SIZE);
}

/** read of /dev/bootpart (`raw_window_read`). */
static int32_t dev_bootpart_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return raw_window_read(&bootpart, e, buf, count);
}

/* A disk's nodes, the whole disk and each partition slot, and the minor
 * from which the nodes past disk 0's first five are numbered
 * (`disk_node_name`). */
#define DISK_NODE_WINDOWS (DISK_PARTS + 1)
#define DISK_MINOR_BASE   64

struct disk_node {
    struct devfs_entry entry; /* first, so an entry pointer is the node */
    char name[16];
    struct disk *disk;
    int part; /* 0 for the whole disk, else the 1-based partition */
    /* Another device over the node's sectors whose transfers /proc/diskstats
     * counts as the node's: the kernel's own view of a boot root it mounted
     * (`devfs_window_alias`), or NULL. */
    const struct block_device *alias;
};

static struct disk_node disk_nodes[DISK_MAX * DISK_NODE_WINDOWS];
static int disk_node_count;
#define DISK_NODES_CAP ((int)(sizeof(disk_nodes) / sizeof(disk_nodes[0])))

/*
 * A slot of the registry of nodes made at run time (devfs.h). The slot
 * keeps its own copy of the node's name and numbers, since a descriptor
 * open on the node points at the slot's entry and outlives the
 * registration; `node` is the registrant's record while it is registered
 * and NULL after. A slot once used is taken again first by a node of the
 * same name, then only when every slot has been used.
 */
struct reg_slot {
    struct devfs_entry entry; /* first, so an entry pointer is the slot */
    char name[DEVFS_NODE_NAME_MAX];
    struct devfs_node *node;
};

static struct reg_slot reg_slots[DEVFS_NODES_MAX];
static int reg_slot_count; /* slots ever used, from the first */

/** The registry's slot `dev` is, or NULL for an entry of the table or a
 * disk node. A slot's operations are its driver's, so its address says. */
static const struct reg_slot *reg_slot_of(const struct devfs_entry *dev)
{
    uintptr_t off = (uintptr_t)dev - (uintptr_t)reg_slots;

    return off < sizeof(reg_slots) ? (const struct reg_slot *)dev : 0;
}

/*
 * The devices root gave an owner, group or mode (`devfs_setattr`), as udev
 * gives Linux's devtmpfs nodes theirs; every other device is root's with
 * its entry's mode. A record whose `dev` is NULL is free.
 */
struct devfs_owned {
    const struct devfs_entry *dev;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode; /* the nine permission bits */
};

static struct devfs_owned owned[DEVFS_OWNED_MAX];

/** The record of `dev`, or NULL for a device that has none. A reader that
 * races a chmod or chown sees the bits before it or after it. */
static const struct devfs_owned *owned_of(const struct devfs_entry *dev)
{
    int i;

    for (i = 0; i < DEVFS_OWNED_MAX; i++) {
        if (owned[i].dev == dev) {
            return &owned[i];
        }
    }
    return NULL;
}

/** Drop the record of `dev`, which a node registered anew does not keep. */
static void owned_drop(const struct devfs_entry *dev)
{
    uint32_t flags = kirq_save();
    int i;

    for (i = 0; i < DEVFS_OWNED_MAX; i++) {
        if (owned[i].dev == dev) {
            owned[i].dev = NULL;
        }
    }
    kirq_restore(flags);
}

/** The permission bits of `dev`: its record's, else its entry's. */
static uint32_t dev_mode(const struct devfs_entry *dev)
{
    const struct devfs_owned *o = owned_of(dev);

    return o ? o->mode : dev->mode;
}

static const struct file_ops diskwin_ops;

/** The disk node `dev` is, or NULL for a device of the table. */
static const struct disk_node *disk_node_of(const struct devfs_entry *dev)
{
    if (dev->ops != &diskwin_ops) {
        return 0;
    }
    return (const struct disk_node *)dev;
}

/** The node behind a descriptor open on a disk window, or NULL for a
 * descriptor open on anything else. `dev_ioctl` is reached by every device,
 * not only the windows, so the entry's operations say whether its record is
 * a node: a plain `devfs_entry` read as one reads past the table it is in. */
static const struct disk_node *disk_node_of_fd(const fd_entry_t *e)
{
    return e->dev ? disk_node_of(e->dev) : 0;
}

/**
 * The window a node names: the whole medium, or one of its partitions.
 *
 * `node` is the caller's to check; every caller of this that took it from a
 * descriptor answers `ENOTTY` or `EIO` for a descriptor that names no node.
 */
static struct disk_window *disk_node_window(const struct disk_node *node)
{
    if (node->part == 0) {
        return &node->disk->whole;
    }
    return &node->disk->views[node->part - 1];
}

static int32_t raw_window_read_bytes(struct disk_window *w, fd_entry_t *e, char __user *buf,
                                     uint32_t count);

/**
 * Read a disk window at its descriptor's byte offset (`raw_window_read_bytes`),
 * or -EIO for a descriptor of no node or a window not published.
 *
 * Reads accept arbitrary byte ranges. Writes require whole sectors and are
 * available only in recovery builds.
 */
static int32_t dev_disk_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    const struct disk_node *node = disk_node_of_fd(e);
    struct disk_window *w;

    if (!node) {
        return -EIO;
    }
    w = disk_node_window(node);
    if (!w->present) {
        return -EIO; /* a stick pulled out from under the reader */
    }
    return raw_window_read_bytes(w, e, buf, count);
}

/**
 * `BLKRRPART` on a whole-disk node: read the table again, for a table
 * rewritten through the node or on another machine, refused while a
 * partition is mounted or another descriptor is open on the disk
 * (`disk_rescan`). -ENOTTY for a descriptor that is not of a whole disk.
 */
static int32_t dev_disk_rescan(fd_entry_t *e)
{
    const struct disk_node *node = disk_node_of_fd(e);

    if (!node || node->part != 0) {
        return -ENOTTY;
    }
    return disk_rescan(node->disk);
}

/**
 * Write whole sectors anywhere on a disk (`raw_window_write`), in a recovery
 * build only; -EIO for a descriptor of no node.
 *
 * Such a write can overwrite any sector of the disk, the boot loader and
 * every file system among them, so which kernel has it is decided when the
 * kernel is built and not when it boots. An ordinary build compiles the
 * refusal, -EPERM, and has no path around it; a `-DRACH2_RECOVERY` build
 * compiles the body. Each has a banner naming which it is
 * (block/recovery.c), and `check_recovery.sh` reads it off the artifact in
 * both directions.
 */
#ifdef RACH2_RECOVERY
static int32_t dev_disk_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    const struct disk_node *node = disk_node_of_fd(e);

    if (!node) {
        return -EIO;
    }
    return raw_window_write(disk_node_window(node), e, buf, count);
}
#else
static int32_t dev_disk_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -EPERM;
}
#endif

/**
 * Read any byte range from a disk's window.
 *
 * The boot windows take whole sectors only, because their readers are the
 * reflash tools and a partial sector there is a mistake. A disk's nodes are
 * read by `blkid`, whose probers ask for the size of a superblock structure
 * at whatever offset it is, and a block device node on Linux answers
 * that. So the covering sectors are read into the staging buffer and the
 * bytes asked for are copied out of it, one batch at a time; the end is a
 * short read, as a regular file's is. Returns the bytes read, 0 at or past
 * the end, or -EFAULT or the request's error when nothing was read, and
 * moves `raw_offset` past what was read.
 */
static int32_t raw_window_read_bytes(struct disk_window *w, fd_entry_t *e, char __user *buf,
                                     uint32_t count)
{
    block_device_t *part = &w->part.device;
    uint64_t off = e->ofd->raw_offset;
    uint64_t total = part->sector_count * BLOCK_SECTOR_SIZE;
    uint32_t done = 0;
    uint8_t *batch;
    uint32_t batch_sectors;
    int rc = 0;

    if (off >= total) {
        return 0;
    }
    if ((uint64_t)count > total - off) {
        count = (uint32_t)(total - off);
    }
    batch = raw_batch_take(&batch_sectors, count / BLOCK_SECTOR_SIZE, part);
    while (done < count) {
        uint64_t at = off + done;
        uint64_t lba = at / BLOCK_SECTOR_SIZE;
        uint32_t skip = (uint32_t)(at % BLOCK_SECTOR_SIZE);
        uint32_t left = count - done;
        uint32_t sectors = (skip + left + BLOCK_SECTOR_SIZE - 1u) / BLOCK_SECTOR_SIZE;
        uint32_t take;

        if (sectors > batch_sectors) {
            sectors = batch_sectors;
        }
        take = sectors * BLOCK_SECTOR_SIZE - skip;
        if (take > left) {
            take = left;
        }
        raw_calls++;
        rc = block_request(part, 0, lba, sectors, batch);
        if (rc != 0) {
            break;
        }
        if (copy_to_user((void __user *)(buf + done), batch + skip, take) != 0) {
            if (done == 0) {
                raw_batch_put(batch, batch_sectors);
                return -EFAULT;
            }
            break;
        }
        done += take;
    }
    raw_batch_put(batch, batch_sectors);
    if (done == 0) {
        return rc;
    }
    e->ofd->raw_offset = off + done;
    return (int32_t)done;
}

/** write of /dev/bootpart (`raw_window_write`). */
static int32_t dev_bootpart_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return raw_window_write(&bootpart, e, buf, count);
}

/** read of /dev/grubcore (`raw_window_read`). */
static int32_t dev_grubcore_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return raw_window_read(&grubcore, e, buf, count);
}

/** write of /dev/grubcore (`raw_window_write`). */
static int32_t dev_grubcore_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return raw_window_write(&grubcore, e, buf, count);
}

/*
 * One writer at a time across every raw window on the disk.
 *
 * A reflash is a sequence of writes at an offset each descriptor keeps for
 * itself. Two of them at once, over ranges that overlap, each write their whole
 * declared length and each report success, and what is on the platter
 * afterwards is neither image. Nothing detects that: the bytes are a mixture,
 * and a hash taken by either writer disagrees with what it sent.
 *
 * The three windows are not three disks. `/dev/disk0` is the whole drive,
 * `/dev/bootpart` is a partition inside it and `/dev/grubcore` is the gap
 * before the first one, so a whole-disk write and a kernel install address the
 * same sectors by different names. One reservation covers all three, and the
 * second writer gets EBUSY rather than a turn. Readers are not counted: a read
 * alongside a write may return a torn region, and a repair hashes the region
 * after its write has completed.
 */
static struct disk *window_disk(const struct devfs_entry *dev);

/** The last close of a raw window's description: one open for writing gives
 * back its disk's writer reservation. */
static void raw_close(fd_entry_t *e)
{
    if (e->flags & FD_FLAG_WRITE) {
        struct disk *d = window_disk(e->dev);

        if (d) {
            d->writer_held = 0;
        }
    }
}

/*
 * One open of /dev/grubcore at a time.
 *
 * Replacing the boot loader is a sequence: read which tail is in use, write the
 * other one, read it back, then rewrite the sector that selects between them.
 * Two of those running at once can each verify their own tail and then leave
 * the selector naming one image with the other one's length, and the machine
 * does not boot. Nothing recovers from that remotely, because GRUB is what
 * loads recovery, so the second open is refused, not serialized. The
 * flag clears when the last descriptor for the open file closes, which process
 * exit does, so a tool that dies mid-install does not lock the device out.
 */
static int grubcore_held;

/** The last close of /dev/grubcore: the device may be opened again. */
static void dev_grubcore_close(fd_entry_t *e)
{
    grubcore_held = 0;
    raw_close(e);
}

/** The last close of /dev/bootpart (`raw_close`). */
static void dev_bootpart_close(fd_entry_t *e)
{
    raw_close(e);
}

/** The last close of a disk node: `raw_close`, and one open fewer on its
 * disk, which `disk_rescan` counts. */
static void dev_disk_close(fd_entry_t *e)
{
    const struct disk_node *node = disk_node_of_fd(e);

    raw_close(e);
    if (node) {
        node->disk->opens--;
    }
}

/*
 * /dev/loop0 to /dev/loop3, and the node that attaches and detaches them.
 *
 * The nodes are always there, as block devices with Linux's loop major, 7,
 * so that util-linux's losetup and mount find a free one and attach a file
 * to it with Linux's ioctls (`loop_node_ioctl`). A node reads and writes its
 * device in whole sectors, as a disk node does: blkid reads it to learn the
 * file system an image has, and mkfs writes one. A write is refused while a
 * file system is mounted on the device, since the cache may hold its blocks.
 * The control node takes one line per write: `attach N /path`, `attach -
 * /path` for the first free device, `detach N`; `ro` after `attach`
 * attaches read-only.
 */
static int devfs_loop_index(const struct devfs_entry *dev);

/**
 * A read of a loop device's node into `in`, or with `in` NULL a write from
 * `out`, of `count` bytes at the description's `raw_offset`, which moves
 * past them. Returns the bytes moved, `loop_node_device`'s error, -EPERM for
 * a write through a descriptor not open for writing or by a caller that is
 * not privileged, -EINVAL for a part of a sector, -ENOSPC for a write at or
 * past the end, or -EFAULT or the request's error when nothing moved.
 */
static int32_t loop_node_transfer(fd_entry_t *e, char __user *in, const char __user *out,
                                  uint32_t count)
{
    int write = in == 0;
    int32_t err;
    block_device_t *dev = loop_node_device(devfs_loop_index(e->dev), write, &err);
    uint64_t off = e->ofd->raw_offset, lba;
    uint32_t want, done = 0, chunk, sectors;
    uint8_t *batch;
    int rc = 0;

    if (!dev) {
        return err;
    }
    if (write && (!(e->flags & FD_FLAG_WRITE) || !proc_privileged(current_proc))) {
        return -EPERM;
    }
    if (off % BLOCK_SECTOR_SIZE != 0 || count % BLOCK_SECTOR_SIZE != 0) {
        return -EINVAL;
    }
    lba = off / BLOCK_SECTOR_SIZE;
    if (lba >= dev->sector_count) {
        return write ? -ENOSPC : 0;
    }
    want = count / BLOCK_SECTOR_SIZE;
    if ((uint64_t)want > dev->sector_count - lba) {
        want = (uint32_t)(dev->sector_count - lba);
    }
    batch = raw_batch_take(&sectors, want, dev);
    while (done < want && rc == 0) {
        chunk = want - done < sectors ? want - done : sectors;
        if (write && copy_from_user(batch, (const void __user *)(out + done * BLOCK_SECTOR_SIZE),
                                    chunk * BLOCK_SECTOR_SIZE) != 0) {
            rc = -EFAULT;
            break;
        }
        rc = block_request(dev, write, lba + done, chunk, batch);
        if (rc == 0 && !write &&
            copy_to_user((void __user *)(in + done * BLOCK_SECTOR_SIZE), batch,
                         chunk * BLOCK_SECTOR_SIZE) != 0) {
            rc = -EFAULT;
        }
        if (rc == 0) {
            done += chunk;
        }
    }
    raw_batch_put(batch, sectors);
    if (done == 0) {
        return rc;
    }
    e->ofd->raw_offset = off + (uint64_t)done * BLOCK_SECTOR_SIZE;
    return (int32_t)(done * BLOCK_SECTOR_SIZE);
}

/** read of a loop device's node (`loop_node_transfer`). */
static int32_t loop_node_read(fd_entry_t *entry, char __user *buffer, uint32_t count)
{
    return loop_node_transfer(entry, buffer, 0, count);
}

/** write of a loop device's node (`loop_node_transfer`). */
static int32_t loop_node_write(fd_entry_t *entry, const char __user *buffer, uint32_t count)
{
    return loop_node_transfer(entry, 0, buffer, count);
}

/**
 * Linux's loop ioctls on a device's node. LOOP_SET_FD and LOOP_CONFIGURE
 * attach the file a descriptor of the caller has open, named by the path
 * `/proc/self/fd` gives it, writable when both descriptors are open for
 * writing; the rest are `loop_ioctl`'s, and a request that is not a loop
 * one is the disk ioctls' of every device node. The two are -EPERM for a
 * caller that is not privileged, -EFAULT, -EINVAL for a block size or a
 * flag they do not take, -EBADF for a descriptor not open on a file,
 * -ENAMETOOLONG for a path too long to attach, or `loop_attach_file`'s
 * error.
 */
static int32_t loop_node_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    uint8_t config[LOOP_CONFIG_SIZE];
    char path[256];
    uint32_t fd = (uint32_t)arg, flags = 0, block_size = 0;
    fd_entry_t *file;
    int32_t rc;
    int len;

    if (request != LOOP_SET_FD && request != LOOP_CONFIGURE) {
        rc = loop_ioctl(devfs_loop_index(e->dev), proc_privileged(current_proc), request, arg);
        return rc == -ENOTTY ? dev_ioctl(e, request, arg) : rc;
    }
    if (!proc_privileged(current_proc)) {
        return -EPERM;
    }
    if (request == LOOP_CONFIGURE) {
        if (copy_from_user(config, (const void __user *)arg, LOOP_CONFIG_SIZE) != 0) {
            return -EFAULT;
        }
        k_memcpy(&fd, config, 4);
        k_memcpy(&block_size, config + 4, 4);
        if ((block_size != 0 && block_size != BLOCK_SECTOR_SIZE) ||
            loop_info_flags(config + LOOP_CONFIG_INFO, &flags) != 0) {
            return -EINVAL;
        }
    }
    file = fd_get(current_proc->fds, (int)fd);
    if (!file || file->type != FD_TYPE_FILE) {
        return -EBADF;
    }
    len = procfs_readlink_fd((int)current_proc->pid, (int)fd, path, (int)sizeof(path));
    if (len < 0) {
        return len;
    }
    if (len >= LOOP_PATH_MAX) {
        return -ENAMETOOLONG;
    }
    return loop_attach_file(devfs_loop_index(e->dev), path,
                            (file->flags & FD_FLAG_WRITE) && (e->flags & FD_FLAG_WRITE), flags);
}

/** The last reference of a description of a loop device's node closed. */
static void loop_node_close(fd_entry_t *e)
{
    loop_node_closed(devfs_loop_index(e->dev));
}

/** write of /dev/loop-control: one line of the commands above
 * (`loop_control_write`). Returns the whole count, -EINVAL for an empty
 * write or one too long for a line, -EFAULT, or the command's error. */
static int32_t loopctl_write(fd_entry_t *entry, const char __user *buffer, uint32_t count)
{
    char line[LOOP_PATH_MAX + 16];
    int rc;

    (void)entry;
    if (count == 0 || count >= sizeof(line)) {
        return -EINVAL;
    }
    if (copy_from_user(line, buffer, count) != 0) {
        return -EFAULT;
    }
    rc = loop_control_write(line, count);
    return rc < 0 ? (int32_t)rc : (int32_t)count;
}

static const struct file_ops loop_ops = {
    loop_node_read, loop_node_write, dev_poll, loop_node_ioctl, loop_node_close, NULL, NULL, 0};

/** write of /dev/framefail: the count its first 16 bytes begin with arms
 * the test switch of the frame allocator (`paging_framefail_arm`). Returns
 * the count, cut to 16, -EFAULT, -EINVAL for a write that does not begin
 * with a digit, or -EPERM unless `rach2.framefail` is on the boot line. */
static int32_t framefail_write(fd_entry_t *entry, const char __user *buffer, uint32_t count)
{
    char digits[16];
    uint32_t n = 0, i;

    (void)entry;
    if (count > sizeof(digits)) {
        count = sizeof(digits);
    }
    if (copy_from_user(digits, buffer, count) != 0) {
        return -EFAULT;
    }
    for (i = 0; i < count && digits[i] >= '0' && digits[i] <= '9'; i++) {
        n = n * 10u + (uint32_t)(digits[i] - '0');
    }
    if (i == 0) {
        return -EINVAL;
    }
    if (paging_framefail_arm(n) != 0) {
        return -EPERM;
    }
    return (int32_t)count;
}

static const struct file_ops framefail_ops = {
    dev_eof_read, framefail_write, dev_poll, dev_ioctl, NULL, NULL, NULL, 0};

/** LOOP_CTL_GET_FREE, LOOP_CTL_ADD and LOOP_CTL_REMOVE, which losetup and
 * mount make before they attach a file (`loop_control_ioctl`); another
 * request is the disk ioctls' (`dev_ioctl`). */
static int32_t loopctl_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    int32_t rc = loop_control_ioctl(request, arg);

    return rc == -ENOTTY ? dev_ioctl(e, request, arg) : rc;
}

static const struct file_ops loopctl_ops = {dev_eof_read, loopctl_write, dev_poll, loopctl_ioctl,
                                            NULL,         NULL,          NULL,     0};

/* /dev/mem's size: the physical addresses 32 bits reach; and the first
 * megabyte, which it reaches whoever owns it, firmware's tables among it. */
#define DEVMEM_SIZE 0x100000000ull
#define DEVMEM_LOW  0x100000u

/**
 * Read /dev/mem into `to`, or with `from` not NULL write it from `from`, at
 * the description's `raw_offset`, which moves past what was moved. /dev/mem
 * is the physical address space as a raw device of 4 GiB, which a
 * privileged caller reads and writes as Linux's with STRICT_DEVMEM:
 * firmware's frames, a device's and those past the top of memory are the
 * caller's (`paging_frame_is_device`), and so is the first megabyte, which
 * the kernel loaded above it leaves to firmware. A transfer stops at 4 GiB
 * and at another frame of RAM, which the allocator owns. Returns the bytes
 * moved, -EPERM for a caller that is not privileged or a first frame of the
 * allocator's, or -EFAULT or -ENOMEM when nothing was moved.
 *
 * The bytes pass through a buffer on the stack, so the temporary mapping is
 * not held across a copy from or to the caller, which may fault.
 */
static int32_t dev_mem_rw(fd_entry_t *e, char __user *to, const char __user *from, uint32_t count)
{
    uint8_t bounce[256];
    uint64_t off = e->ofd->raw_offset;
    uint32_t done = 0;
    int write = from != NULL;

    if (!proc_privileged(current_proc)) {
        return -EPERM;
    }
    while (done < count && off + done < DEVMEM_SIZE) {
        uintptr_t at = (uintptr_t)(off + done);
        uint32_t chunk = count - done;
        uint32_t room = PAGE_SIZE - (uint32_t)(at & (PAGE_SIZE - 1u));
        uint8_t *map;

        chunk = chunk < room ? chunk : room;
        chunk = chunk < sizeof(bounce) ? chunk : (uint32_t)sizeof(bounce);
        if (!paging_frame_is_device(at) && at >= DEVMEM_LOW) {
            if (done == 0) {
                return -EPERM;
            }
            break;
        }
        if (write && copy_from_user(bounce, from + done, chunk) != 0) {
            return done > 0 ? (int32_t)done : -EFAULT;
        }
        map = kmap_temp(at & ~(uintptr_t)(PAGE_SIZE - 1u));
        if (!map) {
            return done > 0 ? (int32_t)done : -ENOMEM;
        }
        if (write) {
            k_memcpy(map + (at & (PAGE_SIZE - 1u)), bounce, chunk);
        } else {
            k_memcpy(bounce, map + (at & (PAGE_SIZE - 1u)), chunk);
        }
        kunmap_temp(map);
        if (!write && copy_to_user(to + done, bounce, chunk) != 0) {
            return done > 0 ? (int32_t)done : -EFAULT;
        }
        done += chunk;
    }
    e->ofd->raw_offset = off + done;
    return (int32_t)done;
}

/** read of /dev/mem (`dev_mem_rw`). */
static int32_t dev_mem_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return dev_mem_rw(e, buf, NULL, count);
}

/** write of /dev/mem (`dev_mem_rw`). */
static int32_t dev_mem_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return dev_mem_rw(e, NULL, buf, count);
}

/** The `page` of a /dev/mem mapping: page `pgoff` is the frame of that
 * number, which `dev_mem_mmap` allowed. */
static int dev_mem_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    (void)obj;
    *phys = (uintptr_t)pgoff * PAGE_SIZE;
    return 0;
}

/** The `put` of a /dev/mem mapping: the mapping holds no object. */
static void dev_mem_put(void *obj)
{
    (void)obj;
}

static const struct vm_device_ops dev_mem_vm_ops = {dev_mem_page, dev_mem_put, NULL, 0};

/**
 * mmap of /dev/mem: a shared mapping of the frames from the page offset,
 * uncached, for a privileged caller, of the frames `dev_mem_rw` reaches,
 * all of them below 4 GiB. Its pages are mapped as they are touched. EINVAL
 * for a private mapping, EPERM for another caller or another frame, ENOMEM
 * with no memory for the region's record.
 */
static int32_t dev_mem_mmap(fd_entry_t *e, struct vma *vma)
{
    uintptr_t pages = (vma->end - vma->start) / PAGE_SIZE;
    uintptr_t i;

    (void)e;
    if (vma->type != VM_SHARED) {
        return -EINVAL;
    }
    if (!proc_privileged(current_proc) || pages > DEVMEM_SIZE / PAGE_SIZE ||
        vma->pgoff > DEVMEM_SIZE / PAGE_SIZE - pages) {
        return -EPERM;
    }
    for (i = 0; i < pages; i++) {
        uintptr_t at = ((uintptr_t)vma->pgoff + i) * PAGE_SIZE;

        if (at >= DEVMEM_LOW && !paging_frame_is_device(at)) {
            return -EPERM;
        }
    }
    return vm_device_map(vma, &dev_mem_vm_ops, NULL, VM_PROT_UC);
}

static const struct file_ops mem_ops = {dev_mem_read, dev_mem_write, dev_poll, dev_ioctl,
                                        NULL,         dev_mem_mmap,  NULL,     0};

/** ioctl of /dev/null, /dev/zero and /dev/full: -ENOTTY for every request,
 * which is `dev_ioctl`'s answer for a device with no size, with no state
 * read, as Linux's have no ioctl (drivers/char/mem.c:649-688). */
static int32_t dev_notty_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    (void)e;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

/* /dev/null, /dev/zero and /dev/full read, write, poll and answer an ioctl
 * from no state but the caller's buffer, so their calls run without the big
 * lock (FILE_OPS_UNLOCKED), as Linux's drivers/char/mem.c takes no lock for
 * them. The random devices keep it: krandom's generator has no lock of its
 * own. */
static const struct file_ops null_ops = {dev_eof_read, dev_sink_write, dev_poll, dev_notty_ioctl,
                                         NULL,         NULL,           NULL,     FILE_OPS_UNLOCKED};
static const struct file_ops zero_ops = {
    dev_zero_read, dev_sink_write, dev_poll, dev_notty_ioctl, NULL, NULL, NULL, FILE_OPS_UNLOCKED};
static const struct file_ops full_ops = {
    dev_zero_read, dev_full_write, dev_poll, dev_notty_ioctl, NULL, NULL, NULL, FILE_OPS_UNLOCKED};
static const struct file_ops rand_ops = {dev_rand_read, dev_sink_write, dev_poll, dev_ioctl,
                                         NULL,          NULL,           NULL,     0};

/**
 * read of /dev/bootlog: the kernel's records in its ring, from the boot and
 * from whatever the machine has said since, and not the debug shell's own
 * output. It lets a question about a boot be asked after that boot, over
 * the network, by somebody who arranged nothing in advance: the screen
 * scrolls the log past in under a second, netconsole reaches only a capture
 * that was already running when the power came on, and holding the screen
 * helps only a person at the machine.
 *
 * The description's offset counts from the oldest record the ring still
 * holds, which is what `klog_read` takes, and an offset past 4 GiB is the
 * end. Records may be evicted between chunk copies, even within one read, so
 * the bytes returned are not a snapshot. A fault after some bytes were
 * copied is a short read: those bytes are returned and the offset moves past
 * them; a fault before any is -EFAULT.
 */
static int32_t dev_bootlog_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    char chunk[256];
    uint32_t done = 0;

    if (e->ofd->offset > 0xFFFFFFFFu) {
        return 0;
    }
    while (done < count) {
        uint32_t want = count - done;
        uint32_t n;

        if (want > sizeof(chunk)) {
            want = (uint32_t)sizeof(chunk);
        }
        n = klog_read(chunk, want, (uint32_t)e->ofd->offset + done);
        if (n == 0) {
            break;
        }
        if (copy_to_user((void __user *)(buf + done), chunk, n) != 0) {
            e->ofd->offset += done;
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        done += n;
    }
    e->ofd->offset += done;
    return (int32_t)done;
}

/**
 * read of /dev/screenlog, which says how much of the kernel's record the
 * screen shows: the level as one digit and a newline, from the description's
 * offset. A write of one digit sets it. The ring, the serial port and
 * netconsole are not affected, so nothing a later reader wants is lost by
 * turning the screen down. It exists because the screen is shared: a
 * program that is talking to a person, `rach2-install` above all, has no
 * other way to stop the kernel's routine notes arriving in the middle of its
 * questions.
 */
static int32_t dev_screenlog_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    char text[2];

    text[0] = (char)('0' + klog_screen_level());
    text[1] = '\n';
    if (e->ofd->offset >= (int32_t)sizeof(text)) {
        return 0;
    }
    if (count > sizeof(text) - (uint32_t)e->ofd->offset) {
        count = (uint32_t)sizeof(text) - (uint32_t)e->ofd->offset;
    }
    if (copy_to_user((void __user *)buf, text + e->ofd->offset, count) != 0) {
        return -EFAULT;
    }
    e->ofd->offset += (int32_t)count;
    return (int32_t)count;
}

/** write of /dev/screenlog: its first byte, a digit, sets the level, and the
 * whole write is taken, so that a caller that wrote "4\n" is not asked to
 * write the newline again. -EINVAL for a byte that is not a digit, or
 * -EFAULT. */
static int32_t dev_screenlog_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    char digit;

    (void)e;
    if (count == 0) {
        return 0;
    }
    if (copy_from_user(&digit, buf, 1) != 0) {
        return -EFAULT;
    }
    if (digit < '0' || digit > '9') {
        return -EINVAL;
    }
    klog_set_screen_level(digit - '0');
    return (int32_t)count;
}

static const struct file_ops screenlog_ops = {
    dev_screenlog_read, dev_screenlog_write, dev_poll, dev_ioctl, NULL, NULL, NULL, 0};

/*
 * /dev/rtc0: the CMOS clock, which `hwclock` reads and sets through Linux's
 * two ioctls, RTC_RD_TIME and RTC_SET_TIME, with a `struct rtc_time` of nine
 * ints: second, minute, hour, day of the month, month from 0, years since
 * 1900, day of the week, day of the year and the daylight flag. The clock
 * keeps UTC, as the boot reads it. Setting it is root's, -EACCES for anyone
 * else as Linux answers; a time outside 2000 to 2099, which the clock's two
 * year digits cannot hold, is -EINVAL.
 *
 * Its alarm, which `rtcwake` sets, is a date and time, as Linux's rtc core
 * keeps it, which the clock, holding a time of day, raises IRQ 8 at:
 * RTC_WKALM_SET and RTC_WKALM_RD set and read the date and time with
 * whether the alarm is enabled in a `struct rtc_wkalrm`; RTC_ALM_SET sets
 * a time of day for the next day it falls on, disabled, and RTC_ALM_READ
 * reads the date and time; RTC_AIE_ON and RTC_AIE_OFF enable and disable
 * the alarm. An enabled alarm must lie within a day ahead, as on Linux
 * without a day alarm; one not after now comes at once. An alarm comes
 * once, which disables it and keeps its time. A read waits for an alarm and
 * gives an unsigned long, as Linux's: the alarms since the last read, which
 * a set does not clear, in the bits above the low eight, with RTC_AF and
 * RTC_IRQF. An alarm is a wake event of a suspend to idle (`suspend_wake`).
 * The clock's update and periodic interrupts are not given: RTC_UIE_ON is
 * -ENOTTY.
 */
#define RTC_AIE_ON    0x7001u
#define RTC_AIE_OFF   0x7002u
#define RTC_ALM_SET   0x40247007u
#define RTC_ALM_READ  0x80247008u
#define RTC_RD_TIME   0x80247009u
#define RTC_SET_TIME  0x4024700Au
#define RTC_WKALM_SET 0x4028700Fu
#define RTC_WKALM_RD  0x80287010u
#define RTC_AF        0x20u
#define RTC_IRQF      0x80u

/* The alarm's state, as Linux's aie_timer keeps it: the date and time it is
 * for, which a set changes and nothing else, so that RTC_WKALM_RD and
 * RTC_ALM_READ give it back after it has come; whether it is enabled, set
 * and not yet come; and the interrupts since the last read. Until the
 * first use takes the clock's own alarm (`rtc_wake_ready`), the time is
 * none. */
static wait_queue_t rtc_wq;
static volatile uint32_t rtc_alarms;
static rtc_time_t rtc_wake;
static volatile int rtc_wake_on;
static int rtc_wake_ready;
static int rtc_irq_ready; /* the alarm interrupt is attached (`rtc_irq_setup`) */
/* Protects `rtc_alarms` and `rtc_wake_on` against the alarm interrupt, which
 * may come on another processor. */
static struct spinlock rtc_alarm_lock = SPINLOCK_INIT_NAMED("rtc_alarm_lock");

/* How `rtc_alarm_came` judges an alarm: due by the clock, whatever its state;
 * due by the clock and still enabled; or raised by the clock's interrupt,
 * whose status C must show the alarm's flag. */
#define RTC_CAME_DUE    0
#define RTC_CAME_ARMED  1
#define RTC_CAME_RAISED 2

/** Count an alarm that has come, which ends it, wake its readers and end
 * a suspend to idle. The alarm is one-shot, as Linux's aie_timer, whose
 * expiry disables it and keeps its time (drivers/rtc/interface.c:951-955),
 * and the clock's driver turns the interrupt off at the first alarm
 * (rtc-cmos.c:731-733). With RTC_CAME_ARMED or RTC_CAME_RAISED, only an
 * alarm still enabled comes, as Linux's rtc_timer_do_work expires only a
 * timer still queued (interface.c:947-955), so that the interrupt and a
 * second reading of the clock that both find it due count it once. The
 * interrupt reads status C inside `rtc_alarm_lock`, so that no set or
 * disable on another processor comes between that read and the test of the
 * alarm's state: a flag left by an alarm that a set has since replaced is
 * cleared by the set's own read of status C, or read here before the set
 * begins, and never counts as the new alarm. Any context. */
static void rtc_alarm_came(int how)
{
    uint32_t flags = spin_lock_irqsave(&rtc_alarm_lock);
    int came = how == RTC_CAME_RAISED ? rtc_alarm_ack() : 1;

    if (!came || (how != RTC_CAME_DUE && !rtc_wake_on)) {
        spin_unlock_irqrestore(&rtc_alarm_lock, flags);
        return;
    }
    rtc_alarm_interrupts(0);
    rtc_wake_on = 0;
    rtc_alarms++;
    spin_unlock_irqrestore(&rtc_alarm_lock, flags);
    wq_wake_all(&rtc_wq);
    suspend_wake();
}

/** The clock's alarm interrupt: an alarm that may have come. */
static void rtc_alarm_irq(isr_params_t *params)
{
    (void)params;
    rtc_alarm_came(RTC_CAME_RAISED);
}

/** Take the clock's alarm interrupt, once. */
static void rtc_irq_setup(void)
{
    if (!rtc_irq_ready) {
        wait_queue_init(&rtc_wq);
        rtc_alarm_attach(rtc_alarm_irq);
        rtc_irq_ready = 1;
    }
}

/**
 * Take the clock's alarm as the alarm's time on the first use, as Linux
 * does when it registers the clock: the clock's time of day, a field the
 * clock does not hold taken from now, with today's date, or tomorrow's when
 * it is not after now (drivers/rtc/interface.c:292-340), and disabled, its
 * interrupt off (rtc-cmos.c:1066). 0, or -EIO when the clock cannot be
 * read.
 */
static int32_t rtc_wake_setup(void)
{
    rtc_time_t now, t;

    rtc_irq_setup();
    if (rtc_wake_ready) {
        return 0;
    }
    if (rtc_read_clock(&now) != 0) {
        return -EIO;
    }
    t = now;
    rtc_alarm_read(&t);
    if (t.second < 0 || t.second > 59) {
        t.second = now.second;
    }
    if (t.minute < 0 || t.minute > 59) {
        t.minute = now.minute;
    }
    if (t.hour < 0 || t.hour > 23) {
        t.hour = now.hour;
    }
    if (rtc_seconds_of(&t) <= rtc_seconds_of(&now)) {
        rtc_time_of_seconds(rtc_seconds_of(&t) + 86400, &t);
    }
    rtc_alarm_interrupts(0);
    rtc_wake = t;
    rtc_wake_on = 0;
    rtc_wake_ready = 1;
    return 0;
}

/** Disable the alarm, its interrupt off, keeping its time, as Linux's
 * rtc_timer_remove of the last timer (drivers/rtc/interface.c:897-907). */
static void rtc_wake_disable(void)
{
    uint32_t flags = spin_lock_irqsave(&rtc_alarm_lock);

    rtc_alarm_interrupts(0);
    rtc_wake_on = 0;
    spin_unlock_irqrestore(&rtc_alarm_lock, flags);
}

/**
 * Enable the alarm for its time, as Linux's rtc_timer_enqueue does: a time
 * not after now raises it at once (drivers/rtc/interface.c:428-429 and
 * 858-861), and one more than a day less a second ahead, which the CMOS
 * clock without a day alarm cannot hold, is -EINVAL and leaves it disabled
 * (rtc-cmos.c:413-423, interface.c:862-866). An alarm due within a second
 * reads the clock again once it is armed, as Linux's __rtc_set_alarm
 * (interface.c:459-466), since the clock may have reached it between the
 * first reading and the write, and then the CMOS alarm would match only a
 * day later; one that has become due comes. The alarms not yet read are
 * kept, as Linux clears them only at open and read (drivers/rtc/dev.c:34,
 * 158-159). 0, -EINVAL, or -EIO when the clock cannot be read, which leaves
 * the alarm disabled (interface.c:862-866).
 */
static int32_t rtc_wake_enable(void)
{
    rtc_time_t now;
    int64_t at = rtc_seconds_of(&rtc_wake), now_s;
    uint32_t flags;

    if (rtc_read_clock(&now) != 0) {
        return -EIO;
    }
    now_s = rtc_seconds_of(&now);
    if (at <= now_s) {
        rtc_alarm_came(RTC_CAME_DUE);
        return 0;
    }
    if (at - now_s > 86400 - 1) {
        rtc_wake_disable();
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&rtc_alarm_lock);
    rtc_alarm_interrupts(0);
    rtc_alarm_write(&rtc_wake);
    rtc_wake_on = 1;
    rtc_alarm_interrupts(1);
    spin_unlock_irqrestore(&rtc_alarm_lock, flags);
    if (at - now_s <= 1) {
        if (rtc_read_clock(&now) != 0) {
            rtc_wake_disable();
            return -EIO;
        }
        if (rtc_seconds_of(&now) >= at) {
            rtc_alarm_came(RTC_CAME_ARMED);
        }
    }
    return 0;
}

/**
 * Set the alarm to `t`, a valid date and time, enabled or not, as Linux's
 * rtc_set_alarm: an enabled alarm is disabled first, the time is kept
 * whatever comes of enabling it, and then it is enabled when `enabled`
 * says (drivers/rtc/interface.c:494-508). 0, or the error of
 * `rtc_wake_enable`.
 */
static int32_t rtc_wake_set(const rtc_time_t *t, int enabled)
{
    rtc_irq_setup();
    rtc_wake_disable();
    rtc_wake = *t;
    rtc_wake_ready = 1;
    return enabled ? rtc_wake_enable() : 0;
}

/** The wait condition of a read: an alarm has come since the last read. */
static int rtc_alarm_pending(void *arg)
{
    (void)arg;
    return rtc_alarms != 0;
}

/**
 * read(2) of /dev/rtc0: wait for the alarm and give the alarms since the
 * last read above the low eight bits, with RTC_AF and RTC_IRQF, as Linux's
 * `rtc_dev_read` gives them: four bytes to a read of exactly four, and an
 * unsigned long, eight bytes on x86-64, to one of that size or more.
 * Returns the bytes given, -EINVAL for another size, -EAGAIN, -EINTR or
 * -EFAULT.
 */
static int32_t dev_rtc_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    unsigned long data;
    uint32_t word, flags;

    if (count != sizeof(word) && count < sizeof(data)) {
        return -EINVAL;
    }
    rtc_irq_setup();
    while (!rtc_alarms) {
        if (e->ofd->status_flags & OFD_NONBLOCK) {
            return -EAGAIN;
        }
        if (wq_wait_event(&rtc_wq, rtc_alarm_pending, NULL) < 0) {
            return -EINTR;
        }
    }
    flags = spin_lock_irqsave(&rtc_alarm_lock);
    data = (unsigned long)rtc_alarms << 8 | RTC_AF | RTC_IRQF;
    rtc_alarms = 0;
    spin_unlock_irqrestore(&rtc_alarm_lock, flags);
    if (count == sizeof(word)) {
        word = (uint32_t)data;
        return copy_to_user(buf, &word, sizeof(word)) != 0 ? -EFAULT : (int32_t)sizeof(word);
    }
    return copy_to_user(buf, &data, sizeof(data)) != 0 ? -EFAULT : (int32_t)sizeof(data);
}

/** poll of /dev/rtc0: POLLIN among `events` while an alarm has come since
 * the last read, and the alarm's wait queue. */
static short dev_rtc_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    (void)e;
    rtc_irq_setup();
    *wq = &rtc_wq;
    return (short)(events & (rtc_alarms ? POLLIN : 0));
}

int64_t devfs_rtc_wakealarm(void)
{
    return rtc_wake_on ? rtc_seconds_of(&rtc_wake) : -1;
}

int32_t devfs_rtc_set_wakealarm(int64_t at)
{
    rtc_time_t now, t;

    if (at < 0) {
        /* Linux's wakealarm_store disables the alarm with a time five
         * minutes ahead (drivers/rtc/sysfs.c:203-209). */
        if (rtc_read_clock(&now) != 0) {
            return -EIO;
        }
        rtc_time_of_seconds(rtc_seconds_of(&now) + 300, &t);
        return rtc_wake_set(&t, 0);
    }
    if (rtc_time_of_seconds(at, &t) != 0) {
        return -EINVAL; /* a year rtc_valid_tm refuses (drivers/rtc/lib.c:163-164) */
    }
    return rtc_wake_set(&t, 1);
}

/** Whether the Linux `struct rtc_time` `tm` is a valid date and time from
 * 1970, as Linux's rtc_valid_tm decides (drivers/rtc/lib.c:161-172), which
 * every alarm set passes before it changes anything. */
static int rtc_tm_valid(const int32_t *tm)
{
    if (tm[5] < 70 || tm[5] > 0x7fffffffL - 1900 || (uint32_t)tm[4] >= 12 || tm[3] < 1 ||
        tm[3] > rtc_days_in_month(tm[5] + 1900, tm[4] + 1) || (uint32_t)tm[2] >= 24 ||
        (uint32_t)tm[1] >= 60 || (uint32_t)tm[0] >= 60) {
        return 0;
    }
    return 1;
}

/** The Linux `struct rtc_time` `tm`, a valid one (`rtc_tm_valid`), as a
 * time of the clock's calendar. */
static void rtc_from_tm(const int32_t *tm, rtc_time_t *t)
{
    t->second = tm[0];
    t->minute = tm[1];
    t->hour = tm[2];
    t->day = tm[3];
    t->month = tm[4] + 1;
    t->year = tm[5] + 1900;
}

/** The clock's time `t` as a Linux `struct rtc_time` into `tm`. */
static void rtc_to_tm(const rtc_time_t *t, int32_t *tm)
{
    tm[0] = t->second;
    tm[1] = t->minute;
    tm[2] = t->hour;
    tm[3] = t->day;
    tm[4] = t->month - 1;
    tm[5] = t->year - 1900;
    rtc_week_and_year_day(t, &tm[6], &tm[7]);
    tm[8] = 0;
}

/** RTC_WKALM_SET and RTC_WKALM_RD's `struct rtc_wkalrm` at `arg`: whether
 * the alarm is enabled, `pending`, which a read gives as 0, and its date and
 * time, which a read gives back after the alarm has come, as Linux's
 * rtc_read_alarm (drivers/rtc/interface.c:398-401). A set is
 * -EACCES for a caller that is not privileged and -EINVAL for a date and
 * time `rtc_tm_valid` refuses, which leaves the alarm as it was. 0,
 * -EFAULT, -EIO when the clock cannot be read, or the error of
 * `rtc_wake_set`. */
static int32_t dev_rtc_wkalarm(uint32_t request, uintptr_t arg)
{
    uint8_t w[40];
    rtc_time_t t;
    int32_t tm[9], rc;

    if (request == RTC_WKALM_RD) {
        rc = rtc_wake_setup();
        if (rc != 0) {
            return rc;
        }
        k_memset(w, 0, sizeof(w));
        w[0] = (uint8_t)(rtc_wake_on != 0);
        rtc_to_tm(&rtc_wake, tm);
        k_memcpy(w + 4, tm, sizeof(tm));
        return copy_to_user((void __user *)arg, w, sizeof(w)) != 0 ? -EFAULT : 0;
    }
    if (!proc_privileged(current_proc)) {
        return -EACCES;
    }
    if (copy_from_user(w, (const void __user *)arg, sizeof(w)) != 0) {
        return -EFAULT;
    }
    k_memcpy(tm, w + 4, sizeof(tm));
    if (!rtc_tm_valid(tm)) {
        return -EINVAL;
    }
    rtc_from_tm(tm, &t);
    return rtc_wake_set(&t, w[0] != 0);
}

/** write of /dev/rtc0: -EINVAL, as Linux's has no write. */
static int32_t dev_rtc_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -EINVAL;
}

/**
 * RTC_ALM_READ, RTC_ALM_SET, RTC_AIE_ON and RTC_AIE_OFF, as Linux's
 * rtc_dev_ioctl: a read gives the alarm's date and time; a set takes a time
 * of day for today, or tomorrow when it is before now, and leaves the alarm
 * disabled (drivers/rtc/dev.c:268-318); RTC_AIE_ON enables the alarm, which
 * comes at once when its time is not after now (dev.c:347-349,
 * interface.c:557-559), and RTC_AIE_OFF disables it. The last three are a
 * privileged caller's. 0, -EIO when the clock cannot be read, -EACCES,
 * -EFAULT, -EINVAL for a time of day out of range, or the error of
 * `rtc_wake_enable`.
 */
static int32_t dev_rtc_alarm(uint32_t request, uintptr_t arg)
{
    int32_t tm[9], rc;
    rtc_time_t now, t;

    if (request == RTC_ALM_READ) {
        rc = rtc_wake_setup();
        if (rc != 0) {
            return rc;
        }
        rtc_to_tm(&rtc_wake, tm);
        return copy_to_user((void __user *)arg, tm, sizeof(tm)) != 0 ? -EFAULT : 0;
    }
    if (!proc_privileged(current_proc)) {
        return -EACCES;
    }
    rc = rtc_wake_setup();
    if (rc != 0) {
        return rc;
    }
    if (request == RTC_AIE_ON) {
        return rtc_wake_on ? 0 : rtc_wake_enable();
    }
    if (request == RTC_AIE_OFF) {
        rtc_wake_disable();
        return 0;
    }
    if (copy_from_user(tm, (const void __user *)arg, sizeof(tm)) != 0) {
        return -EFAULT;
    }
    if (rtc_read_clock(&now) != 0) {
        return -EIO;
    }
    tm[3] = now.day;
    tm[4] = now.month - 1;
    tm[5] = now.year - 1900;
    if (!rtc_tm_valid(tm)) {
        return -EINVAL;
    }
    rtc_from_tm(tm, &t);
    if (rtc_seconds_of(&t) < rtc_seconds_of(&now)) {
        rtc_time_of_seconds(rtc_seconds_of(&now) + 86400, &now);
        t.day = now.day;
        t.month = now.month;
        t.year = now.year;
    }
    return rtc_wake_set(&t, 0);
}

/** The ioctls of /dev/rtc0 named before its request numbers; -ENOTTY for
 * another. RTC_RD_TIME is -EIO when the clock cannot be read, and
 * RTC_SET_TIME -EACCES for a caller that is not privileged and -EINVAL for a
 * month out of range, a year outside 2000 to 2099 or a time the clock
 * refuses. */
static int32_t dev_rtc_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    int32_t tm[9];
    rtc_time_t t;

    (void)e;
    if (request == RTC_RD_TIME) {
        if (rtc_read_clock(&t) != 0) {
            return -EIO;
        }
        rtc_to_tm(&t, tm);
        return copy_to_user((void __user *)arg, tm, sizeof(tm)) != 0 ? -EFAULT : 0;
    }
    if (request == RTC_WKALM_SET || request == RTC_WKALM_RD) {
        return dev_rtc_wkalarm(request, arg);
    }
    if (request == RTC_ALM_READ || request == RTC_ALM_SET || request == RTC_AIE_ON ||
        request == RTC_AIE_OFF) {
        return dev_rtc_alarm(request, arg);
    }
    if (request == RTC_SET_TIME) {
        if (!proc_privileged(current_proc)) {
            return -EACCES;
        }
        if (copy_from_user(tm, (const void __user *)arg, sizeof(tm)) != 0) {
            return -EFAULT;
        }
        if (tm[4] < 0 || tm[4] > 11 || tm[5] < 100 || tm[5] > 199) {
            return -EINVAL;
        }
        t.second = tm[0];
        t.minute = tm[1];
        t.hour = tm[2];
        t.day = tm[3];
        t.month = tm[4] + 1;
        t.year = tm[5] + 1900;
        return rtc_write_clock(&t) != 0 ? -EINVAL : 0;
    }
    return -ENOTTY;
}

static const struct file_ops rtc_ops = {dev_rtc_read, dev_rtc_write, dev_rtc_poll, dev_rtc_ioctl,
                                        NULL,         NULL,          NULL,         0};

/* The event devices' operations are evdev.c's, the mixer's mixer.c's and the
   audio device's dsp.c's. */
static const struct file_ops evdev_ops = {evdev_read,  evdev_write, evdev_poll, evdev_ioctl,
                                          evdev_close, NULL,        NULL,       0};
/* /dev/wsim: the medium of a software radio (wsim(4)). */
static const struct file_ops wsim_ops = {wsim_read,  wsim_write, wsim_poll, wsim_ioctl,
                                         wsim_close, NULL,       NULL,      0};
static const struct file_ops mixer_ops = {NULL, NULL, dev_poll, mixer_ioctl, NULL, NULL, NULL, 0};
static const struct file_ops dsp_ops = {dspdev_read,  dspdev_write, dspdev_poll, dspdev_ioctl,
                                        dspdev_close, NULL,         NULL,        0};
/* /dev/rfkill's are rfkill.c's. */
static const struct file_ops rfkill_ops = {rfkill_read,  rfkill_write, rfkill_poll, rfkill_ioctl,
                                           rfkill_close, NULL,         NULL,        0};

static const struct file_ops grubcore_ops = {
    dev_grubcore_read, dev_grubcore_write, dev_poll, dev_ioctl, dev_grubcore_close, NULL, NULL, 0};
static const struct file_ops bootpart_ops = {
    dev_bootpart_read, dev_bootpart_write, dev_poll, dev_ioctl, dev_bootpart_close, NULL, NULL, 0};
static const struct file_ops bootlog_ops = {
    dev_bootlog_read, dev_sink_write, dev_poll, dev_ioctl, NULL, NULL, NULL, 0};
static const struct file_ops diskwin_ops = {dev_disk_read,  dev_disk_write, dev_poll, dev_ioctl,
                                            dev_disk_close, NULL,           NULL,     0};

/** Whether `dev` is a raw window a write may reach, for the writer
 * reservation: the whole disks and the two boot windows in every build, the
 * partitions in the recovery build. */
static int raw_write_window(const struct devfs_entry *dev)
{
    const struct disk_node *node = disk_node_of(dev);

    if (node) {
        return node->part == 0 ? 1 : recovery_publishes_disk();
    }
    return dev->ops == &bootpart_ops || dev->ops == &grubcore_ops;
}

/** The raw window of `dev`, a disk's or one of the two boot windows, or
 * NULL. */
static struct disk_window *raw_window_of(const struct devfs_entry *dev)
{
    const struct disk_node *node = disk_node_of(dev);

    if (node) {
        return disk_node_window(node);
    }
    if (dev->ops == &bootpart_ops) {
        return &bootpart;
    }
    if (dev->ops == &grubcore_ops) {
        return &grubcore;
    }
    return 0;
}

/** The disk a raw window is on, for its writer reservation, or NULL. */
static struct disk *window_disk(const struct devfs_entry *dev)
{
    const struct disk_node *node = disk_node_of(dev);
    struct disk_window *w;

    if (node) {
        return node->disk;
    }
    w = raw_window_of(dev);
    if (!w || !w->present) {
        return 0;
    }
    return disk_of(w->part.backing);
}

/*
 * The table of devices. The memory devices have Linux's numbers (major 1),
 * the terminals theirs (major 5) and the loop devices theirs (major 7), so
 * stat reports a faithful rdev; the rest are in the block Linux leaves for
 * local use, majors 240 and 254. A device with nothing to release has a
 * NULL close, and a descriptor's `dev` points at its entry, which fstat
 * reads. `mode` is the nine permission bits stat reports and open enforces,
 * the owner of every device being root:root: /dev/null, /dev/zero,
 * /dev/full, the random devices and the terminals are 0666, as on Linux, and
 * every other device is root's, with group access to /dev/mem and the loop
 * devices. /dev/mixer and /dev/dsp are 0666 so that the console's user sets
 * the volume and plays sound, as a desktop's ACL gives them on Linux, the
 * event devices of /dev/input are root's, and /dev/rfkill, at Linux's 10:242,
 * is 0644, as udev makes it, so that anyone reads the radio switches and
 * root changes them. `bootpart` and `grubcore` are
 * present only while published, the mixer while an audio driver offers its
 * volume, dsp while it offers streams, and an event device while its input
 * device is registered, as the disk nodes after the table are, which is why
 * the listing goes through `devfs_dirent` and skips an absent device instead
 * of stopping at it: a machine that has a later one and not an earlier one
 * would otherwise have a device it could open and could not list.
 *
 * An entry with NULL operations is a terminal, which `devfs_open` opens
 * itself: console and tty give an FD_TYPE_CONSOLE descriptor, tty a pty's
 * when the caller's session is on one, and ptmx a new pty pair.
 */
static const struct devfs_entry dev_table[] = {{"mem", 1, 1, 0, 0640, &mem_ops},
                                               {"null", 1, 3, 1, 0666, &null_ops},
                                               {"zero", 1, 5, 1, 0666, &zero_ops},
                                               {"full", 1, 7, 1, 0666, &full_ops},
                                               {"random", 1, 8, 0, 0666, &rand_ops},
                                               {"urandom", 1, 9, 0, 0666, &rand_ops},
                                               {"console", 5, 1, 0, 0666, NULL},
                                               {"tty", 5, 0, 0, 0666, NULL},
                                               {"ptmx", 5, 2, 0, 0666, NULL},
                                               {"bootpart", 240, 0, 0, 0600, &bootpart_ops},
                                               {"bootlog", 240, 2, 0, 0400, &bootlog_ops},
                                               {"grubcore", 240, 3, 0, 0600, &grubcore_ops},
                                               {"loop0", 7, 0, 0, 0660, &loop_ops},
                                               {"loop1", 7, 1, 0, 0660, &loop_ops},
                                               {"loop2", 7, 2, 0, 0660, &loop_ops},
                                               {"loop3", 7, 3, 0, 0660, &loop_ops},
                                               {"loop-control", 240, 36, 0, 0600, &loopctl_ops},
                                               {"framefail", 240, 37, 0, 0600, &framefail_ops},
                                               {"screenlog", 240, 38, 0, 0600, &screenlog_ops},
                                               {"wsim", 240, 39, 0, 0600, &wsim_ops},
                                               {"rtc0", 254, 0, 0, 0600, &rtc_ops},
                                               {"mixer", 14, 0, 0, 0666, &mixer_ops},
                                               {"dsp", 14, 3, 0, 0666, &dsp_ops},
                                               {"rfkill", 10, 242, 0, 0644, &rfkill_ops},
                                               {"input/event0", 13, 64, 0, 0600, &evdev_ops},
                                               {"input/event1", 13, 65, 0, 0600, &evdev_ops},
                                               {"input/event2", 13, 66, 0, 0600, &evdev_ops},
                                               {"input/event3", 13, 67, 0, 0600, &evdev_ops},
                                               {"input/event4", 13, 68, 0, 0600, &evdev_ops},
                                               {"input/event5", 13, 69, 0, 0600, &evdev_ops},
                                               {"input/event6", 13, 70, 0, 0600, &evdev_ops},
                                               {"input/event7", 13, 71, 0, 0600, &evdev_ops},
                                               {"input/event8", 13, 72, 0, 0600, &evdev_ops},
                                               {"input/event9", 13, 73, 0, 0600, &evdev_ops},
                                               {"input/event10", 13, 74, 0, 0600, &evdev_ops},
                                               {"input/event11", 13, 75, 0, 0600, &evdev_ops},
                                               {"input/event12", 13, 76, 0, 0600, &evdev_ops},
                                               {"input/event13", 13, 77, 0, 0600, &evdev_ops},
                                               {"input/event14", 13, 78, 0, 0600, &evdev_ops},
                                               {"input/event15", 13, 79, 0, 0600, &evdev_ops},
                                               {"input/event16", 13, 80, 0, 0600, &evdev_ops},
                                               {"input/event17", 13, 81, 0, 0600, &evdev_ops},
                                               {"input/event18", 13, 82, 0, 0600, &evdev_ops},
                                               {"input/event19", 13, 83, 0, 0600, &evdev_ops},
                                               {"input/event20", 13, 84, 0, 0600, &evdev_ops},
                                               {"input/event21", 13, 85, 0, 0600, &evdev_ops},
                                               {"input/event22", 13, 86, 0, 0600, &evdev_ops},
                                               {"input/event23", 13, 87, 0, 0600, &evdev_ops},
                                               {"input/event24", 13, 88, 0, 0600, &evdev_ops},
                                               {"input/event25", 13, 89, 0, 0600, &evdev_ops},
                                               {"input/event26", 13, 90, 0, 0600, &evdev_ops},
                                               {"input/event27", 13, 91, 0, 0600, &evdev_ops},
                                               {"input/event28", 13, 92, 0, 0600, &evdev_ops},
                                               {"input/event29", 13, 93, 0, 0600, &evdev_ops},
                                               {"input/event30", 13, 94, 0, 0600, &evdev_ops},
                                               {"input/event31", 13, 95, 0, 0600, &evdev_ops}};

#define DEVFS_NDEV ((int)(sizeof(dev_table) / sizeof(dev_table[0])))

/** The loop device an entry names, or -1 for an entry that is not one. */
static int devfs_loop_index(const struct devfs_entry *dev)
{
    if (dev->ops != &loop_ops) {
        return -1;
    }
    return (int)dev->minor;
}

/** Whether `dev`, an entry of the table or a slot of the registry, is
 * present now: a slot while its node is registered; bootpart and grubcore
 * only while published; the mixer, dsp and an event device only while
 * `mixer_present`, `dspdev_present` and `evdev_present` say; and every
 * other entry always. */
static int devfs_entry_present(const struct devfs_entry *dev)
{
    if (reg_slot_of(dev)) {
        return reg_slot_of(dev)->node != 0;
    }
    if (dev->ops == &evdev_ops) {
        return evdev_present(dev->minor - EVDEV_MINOR_BASE);
    }
    if (dev->ops == &mixer_ops) {
        return mixer_present();
    }
    if (dev->ops == &dsp_ops) {
        return dspdev_present();
    }
    if (dev->ops == &bootpart_ops) {
        return bootpart.present;
    }
    if (dev->ops == &grubcore_ops) {
        return grubcore.present;
    }
    return 1;
}

/**
 * The type bits `stat` reports for an entry.
 *
 * The disk nodes and the loop devices are block devices and everything
 * else here is a character device. The difference matters: `blkid`, `findfs`
 * and `mount LABEL=` walk `/dev` and look at nothing that is not a block
 * device, so a volume reported as a character device cannot be found by
 * its label.
 */
static uint32_t devfs_type_bits(const struct devfs_entry *dev)
{
    return dev->ops == &diskwin_ops || dev->ops == &loop_ops ? 0060000u
                                                             : 0020000u; /* S_IFBLK, S_IFCHR */
}

/* Stable inode namespace for /dev: the directory itself, then one per
 * device. */
#define DEVFS_INO_DIR  2u
#define DEVFS_INO_BASE 16u
#define DEVFS_INO(idx) (DEVFS_INO_BASE + (uint32_t)(idx))

/** Whether the strings `a` and `b` are equal. */
static int name_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Whether disk node `j` exists right now: its window is published. */
static int disk_node_present(int j)
{
    return j >= 0 && j < disk_node_count && disk_nodes[j].disk->present &&
           disk_node_window(&disk_nodes[j])->present;
}

int devfs_disk_node_at(int i, const char **name, uint32_t *major, uint32_t *minor,
                       const struct block_device **dev, const struct block_device **alias)
{
    const struct disk_node *node;
    int k;

    if (i >= disk_node_count && i < disk_node_count + LOOP_DEVICES) {
        *dev = loop_attached_at(i - disk_node_count);
        for (k = 0; k < DEVFS_NDEV && devfs_loop_index(&dev_table[k]) != i - disk_node_count; k++) {
        }
        if (!*dev || k == DEVFS_NDEV) {
            return 0;
        }
        *name = dev_table[k].name;
        *major = dev_table[k].major;
        *minor = dev_table[k].minor;
        *alias = 0;
        return 1;
    }
    if (i < 0 || i >= disk_node_count) {
        return -1;
    }
    node = &disk_nodes[i];
    if (node->part == 0 ? !node->disk->present : !disk_node_present(i)) {
        return 0;
    }
    *name = node->name;
    *major = node->entry.major;
    *minor = node->entry.minor;
    *dev = node->part == 0 ? node->disk->dev : &node->disk->views[node->part - 1].part.device;
    *alias = node->alias;
    return 1;
}

static const struct devfs_entry *devfs_find(const char *name);

int devfs_numbers(const char *path, uint32_t *major, uint32_t *minor)
{
    const struct devfs_entry *dev;

    if (path[0] != '/' || path[1] != 'd' || path[2] != 'e' || path[3] != 'v' || path[4] != '/') {
        return 0;
    }
    dev = devfs_find(path + 5);
    if (!dev) {
        return 0;
    }
    *major = dev->major;
    *minor = dev->minor;
    return 1;
}

int devfs_disk_row(int i, struct devfs_disk_row *row)
{
    const struct block_device *dev, *alias;
    const struct disk_node *node;
    const char *p;
    int got = devfs_disk_node_at(i, &row->name, &row->major, &row->minor, &dev, &alias);

    if (got <= 0) {
        return got;
    }
    if (i >= disk_node_count) {
        row->whole = i;
        row->partno = 0;
        row->start = 0;
        row->sectors = dev->sector_count;
        row->removable = 0;
        row->model = "";
        row->serial = "";
        return 1;
    }
    node = &disk_nodes[i];
    for (row->whole = i; row->whole > 0 && disk_nodes[row->whole].disk == node->disk &&
                         disk_nodes[row->whole].part != 0;
         row->whole--) {
    }
    row->partno = 0;
    for (p = node->name; node->part != 0 && *p; p++) {
        row->partno = *p >= '0' && *p <= '9' ? row->partno * 10 + (*p - '0') : 0;
    }
    row->start = node->part != 0 ? node->disk->views[node->part - 1].part.start_lba : 0;
    row->sectors = dev ? dev->sector_count : 0;
    row->removable = node->disk->removable;
    row->model = node->disk->model;
    row->serial = node->disk->serial;
    return 1;
}

void devfs_window_alias(const struct block_device *view, const struct block_device *alias)
{
    int i;

    for (i = 0; i < disk_node_count; i++) {
        if (disk_node_present(i) && &disk_node_window(&disk_nodes[i])->part.device == view) {
            disk_nodes[i].alias = alias;
        }
    }
}

/** The index `stat` and the listing agree on: the table's entries first,
 * then the disk nodes in the order the disks were registered, then the
 * registry's slots after room for every disk node, so that a slot's
 * inode does not move as disks come. */
static int entry_index(const struct devfs_entry *dev)
{
    const struct disk_node *node = disk_node_of(dev);
    const struct reg_slot *slot = reg_slot_of(dev);

    if (node) {
        return DEVFS_NDEV + (int)(node - disk_nodes);
    }
    if (slot) {
        return DEVFS_NDEV + DISK_NODES_CAP + (int)(slot - reg_slots);
    }
    return (int)(dev - dev_table);
}

/** The device named `name` present now, a table entry or a disk node, or
 * NULL. */
static const struct devfs_entry *devfs_find(const char *name)
{
    int i;
    for (i = 0; i < DEVFS_NDEV; i++) {
        if (name_eq(dev_table[i].name, name)) {
            return devfs_entry_present(&dev_table[i]) ? &dev_table[i] : NULL;
        }
    }
    for (i = 0; i < disk_node_count; i++) {
        if (name_eq(disk_nodes[i].name, name)) {
            return disk_node_present(i) ? &disk_nodes[i].entry : NULL;
        }
    }
    for (i = 0; i < reg_slot_count; i++) {
        if (devfs_entry_present(&reg_slots[i].entry) && name_eq(reg_slots[i].name, name)) {
            return &reg_slots[i].entry;
        }
    }
    return NULL;
}

/** Append the decimal `v` at `*n` in `out`, which has `cap` bytes. */
static void put_decimal(char *out, int *n, int cap, int v)
{
    char digits[12];
    int k = 0;

    do {
        digits[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (k > 0 && *n < cap - 1) {
        out[(*n)++] = digits[--k];
    }
    out[*n] = '\0';
}

/** Name a node `diskN` or `diskNpM`, with the minor the pair has. */
static void disk_node_name(struct disk_node *node, int number, int m)
{
    int n = 0;

    node->name[n++] = 'd';
    node->name[n++] = 'i';
    node->name[n++] = 's';
    node->name[n++] = 'k';
    put_decimal(node->name, &n, (int)sizeof(node->name), number);
    if (m > 0) {
        node->name[n++] = 'p';
        put_decimal(node->name, &n, (int)sizeof(node->name), m);
    }
    if (number == 0 && m == 0) {
        node->entry.minor = 1;
    } else if (number == 0 && m <= 4) {
        node->entry.minor = (uint16_t)(3 + m);
    } else {
        node->entry.minor = (uint16_t)(DISK_MINOR_BASE + number * 16 + m);
    }
}

void devfs_disk_nodes(struct disk *d)
{
    int k;

    if (disk_node_count + DISK_NODE_WINDOWS > (int)(sizeof(disk_nodes) / sizeof(disk_nodes[0]))) {
        klog(KLOG_WARN, "devfs", "disk%d: no nodes: the table is full", d->number);
        return;
    }
    for (k = 0; k < DISK_NODE_WINDOWS; k++) {
        struct disk_node *node = &disk_nodes[disk_node_count++];

        node->disk = d;
        node->part = k;
        disk_node_name(node, d->number, k);
        node->entry.name = node->name;
        node->entry.major = 240;
        node->entry.lseek_zero = 0;
        node->entry.mode = 0400;
        node->entry.ops = &diskwin_ops;
    }
}

void devfs_disk_part_nodes(struct disk *d)
{
    int j, k;

    for (j = 0; j < disk_node_count; j++) {
        struct disk_node *node = &disk_nodes[j];

        if (node->disk != d || node->part == 0) {
            continue;
        }
        k = node->part - 1;
        disk_node_name(node, d->number, k < d->table.count ? d->table.parts[k].number : node->part);
    }
}

/*
 * Claims: a mounted volume's sector range on the physical disk. Two ranges
 * are compared on the device `block_physical` reaches, never on the view
 * they were named through, because /dev/bootpart, /dev/disk0p1 and
 * /dev/disk0 can all describe one range, and under the lag device the
 * partition views and the whole-disk window are on different wrappers of
 * one disk. While a range is claimed a second overlapping mount answers
 * EBUSY, and so does a write-capable open of a raw window over it; while
 * such a descriptor is open, a claim on its range answers EBUSY. The table
 * and the single-writer rule above both decide who may write a range of
 * sectors.
 */
struct claim {
    int used;
    block_device_t *backing;
    uint64_t first;
    uint64_t last;
};

#define DEVFS_CLAIMS 8

static struct claim claims[DEVFS_CLAIMS];

/** Whether the sectors `af` to `al` of `a` and `bf` to `bl` of `b` meet. */
static int ranges_cross(block_device_t *a, uint64_t af, uint64_t al, block_device_t *b, uint64_t bf,
                        uint64_t bl)
{
    return a == b && af <= bl && bf <= al;
}

int devfs_claim(block_device_t *backing, uint64_t first, uint64_t last)
{
    int i;

    struct disk *d = disk_of(backing);

    if (d && d->writer_held && first <= d->writer_last && d->writer_first <= last) {
        return -EBUSY;
    }
    for (i = 0; i < DEVFS_CLAIMS; i++) {
        if (claims[i].used && ranges_cross(backing, first, last, claims[i].backing, claims[i].first,
                                           claims[i].last)) {
            return -EBUSY;
        }
    }
    for (i = 0; i < DEVFS_CLAIMS; i++) {
        if (!claims[i].used) {
            claims[i].used = 1;
            claims[i].backing = backing;
            claims[i].first = first;
            claims[i].last = last;
            return i;
        }
    }
    return -ENOMEM;
}

void devfs_release_claim(int claim)
{
    if (claim >= 0 && claim < DEVFS_CLAIMS) {
        claims[claim].used = 0;
    }
}

int devfs_disk_claimed(const struct disk *d)
{
    int i;

    for (i = 0; i < DEVFS_CLAIMS; i++) {
        if (claims[i].used && claims[i].backing == d->dev) {
            return 1;
        }
    }
    return 0;
}

/** Whether a claim meets the sectors `first` to `last` of `backing`. */
static int claimed(block_device_t *backing, uint64_t first, uint64_t last)
{
    int i;

    for (i = 0; i < DEVFS_CLAIMS; i++) {
        if (claims[i].used && ranges_cross(backing, first, last, claims[i].backing, claims[i].first,
                                           claims[i].last)) {
            return 1;
        }
    }
    return 0;
}

/** The range of the raw window `dev` on the physical device under it, for
 * the claim rules: 0, or -ENXIO for a device that is no raw window or one
 * not published. */
static int window_range(const struct devfs_entry *dev, block_device_t **backing, uint64_t *first,
                        uint64_t *last)
{
    struct disk_window *w = raw_window_of(dev);

    if (!w || !w->present) {
        return -ENXIO;
    }
    *backing = block_physical(w->part.backing);
    *first = w->part.start_lba;
    *last = w->part.start_lba + w->part.device.sector_count - 1;
    return 0;
}

/**
 * A file has no sectors on a disk to claim, so a loop device is its own
 * identity, and only one mount can be placed on it. The whole disk is a
 * mount's source when it holds one volume and no table, as a stick
 * formatted without one does; the claim rules keep a whole-disk mount and a
 * partition's from overlapping.
 */
int devfs_window_of(const char *path, block_device_t **view, block_device_t **backing,
                    uint64_t *first, uint64_t *count)
{
    const struct devfs_entry *dev;
    const struct disk_node *node;
    struct disk_window *w = 0;

    if (path[0] != '/' || path[1] != 'd' || path[2] != 'e' || path[3] != 'v' || path[4] != '/') {
        return -EINVAL;
    }
    dev = devfs_find(path + 5);
    if (!dev) {
        return -ENOENT;
    }
    if (dev->ops == &loop_ops) {
        block_device_t *loop = loop_attached_at(devfs_loop_index(dev));

        if (!loop) {
            return -ENXIO;
        }
        *view = loop;
        *backing = 0;
        *first = 0;
        *count = loop->sector_count;
        return 0;
    }
    node = disk_node_of(dev);
    if (dev->ops == &bootpart_ops) {
        w = &bootpart;
    } else if (node) {
        w = disk_node_window(node);
    }
    if (!w) {
        return -EINVAL;
    }
    if (!w->present) {
        return -ENOENT;
    }
    *view = &w->part.device;
    *backing = block_physical(w->part.backing);
    *first = w->part.start_lba;
    *count = w->part.device.sector_count;
    return 0;
}

/**
 * Whether an open of `dev` with `flags` must wait for another's close: the
 * GRUB core image while it is open, and a raw window opened for writing
 * while its disk has a writer or a mounted volume claims its range.
 */
static int devfs_open_busy(const struct devfs_entry *dev, int flags)
{
    struct disk *d;
    block_device_t *backing;
    uint64_t first, last;

    if (dev->ops == &grubcore_ops && grubcore_held) {
        return 1;
    }
    if ((flags & 3) == 0 || !raw_write_window(dev)) {
        return 0;
    }
    d = window_disk(dev);
    if (d && d->writer_held) {
        return 1;
    }
    return window_range(dev, &backing, &first, &last) == 0 && claimed(backing, first, last);
}

/**
 * Record the open of `dev` on the descriptor `e`, whose flags are set: hold
 * the GRUB core image, make a raw window opened for writing its disk's
 * writer, and count the open of a disk node and of a loop device, which
 * their last close undoes.
 */
static void devfs_note_open(const struct devfs_entry *dev, fd_entry_t *e)
{
    if (dev->ops == &grubcore_ops) {
        grubcore_held = 1;
    }
    if ((e->flags & FD_FLAG_WRITE) && raw_write_window(dev)) {
        struct disk *d = window_disk(dev);
        block_device_t *backing;

        if (d && window_range(dev, &backing, &d->writer_first, &d->writer_last) == 0) {
            d->writer_held = 1;
        }
    }
    if (disk_node_of(dev)) {
        disk_node_of(dev)->disk->opens++;
    }
    if (dev->ops == &loop_ops) {
        loop_node_opened(devfs_loop_index(dev));
    }
}

/**
 * Give the descriptor e of the device dev the state of its own open: the
 * sound device's, an event device's, the radio simulator's, the radio
 * switches' or a registered node's (`devfs_node.open`). Returns 0, at once
 * for a device with no such state, or the open's negative errno.
 */
static int32_t devfs_open_state(const struct devfs_entry *dev, fd_entry_t *e, int flags)
{
    if (dev->ops == &dsp_ops) {
        return dspdev_open(e, flags);
    }
    if (dev->ops == &wsim_ops) {
        return wsim_open(e);
    }
    if (dev->ops == &rfkill_ops) {
        return rfkill_open(e);
    }
    if (dev->ops == &evdev_ops) {
        return evdev_open(e, dev->minor - EVDEV_MINOR_BASE);
    }
    if (reg_slot_of(dev)) {
        struct devfs_node *node = reg_slot_of(dev)->node;

        e->priv = NULL;
        return node->open ? node->open(node, e, flags) : 0;
    }
    return 0;
}

/**
 * /dev/tty follows POSIX: the console when the caller is in the console's
 * session, else the pty slave that has the caller's session, else ENXIO,
 * which is also what dropbear's probe after setsid expects. A job-control
 * shell over a pty opens /dev/tty to set its foreground process group, and
 * given the console instead it would never see itself in the foreground and
 * would stop on SIGTTIN. O_CREAT|O_EXCL is refused before a descriptor or a
 * pty pair is made.
 */
int devfs_open(const char *name, int flags)
{
    const struct devfs_entry *dev;
    fd_entry_t *e;
    int fd;

    dev = devfs_find(name);
    if (!dev) {
        return -ENOENT;
    }
    if ((flags & DEVFS_O_CREAT) && (flags & DEVFS_O_EXCL)) {
        return -EEXIST;
    }
    if (flags & DEVFS_O_DIRECTORY) {
        return -ENOTDIR;
    }
    if (name_eq(name, "ptmx")) {
        return pty_open_master(flags);
    }

    if (dev->ops == NULL && name_eq(name, "tty")) {
        int sid = current_proc ? (int)current_proc->sid : 0;
        if (console_tty.sid == 0 || console_tty.sid != sid) {
            int idx = pty_session_slave(sid);
            if (idx >= 0) {
                return pty_open_slave(idx, flags);
            }
            return -ENXIO; /* the caller has no controlling terminal */
        }
    }

    if (devfs_open_busy(dev, flags)) {
        return -EBUSY;
    }

    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        return fd;
    }

    e = fd_slot(current_proc->fds, fd);
    e->node = NULL;
    e->sock = NULL;
    e->ofd->offset = 0;
    {
        int32_t rc = devfs_open_state(dev, e, flags);

        if (rc != 0) {
            fd_free(current_proc->fds, fd);
            return rc;
        }
    }
    if (dev->ops) {
        e->type = FD_TYPE_DEVICE;
        e->ops = dev->ops;
        e->dev = dev;
    } else {
        e->type = FD_TYPE_CONSOLE;
        e->ops = NULL;
        e->priv = NULL;
        e->ofd->tty_hangups = console_tty.vhangups;
    }

    if ((flags & 3) == 1) {
        e->flags = FD_FLAG_WRITE;
    } else if ((flags & 3) == 2) {
        e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    } else {
        e->flags = FD_FLAG_READ;
    }
    if (flags & DEVFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    if ((flags & DEVFS_O_NONBLOCK) && e->ofd) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    devfs_note_open(dev, e);
    fd_install(current_proc->fds, fd);
    return fd;
}

int devfs_stat(const char *name, uint32_t *mode, uint32_t *rdev, uint32_t *ino)
{
    const struct devfs_entry *dev;

    if (name[0] == '\0') {
        if (mode) {
            *mode = 0040000u | 0755u; /* S_IFDIR */
        }
        if (rdev) {
            *rdev = 0;
        }
        if (ino) {
            *ino = DEVFS_INO_DIR;
        }
        return 0;
    }

    dev = devfs_find(name);
    if (!dev) {
        return -1;
    }
    if (mode) {
        *mode = devfs_type_bits(dev) | dev_mode(dev);
    }
    if (rdev) {
        *rdev = ((uint32_t)dev->major << 8) | (uint32_t)dev->minor;
    }
    if (ino) {
        *ino = DEVFS_INO(entry_index(dev));
    }
    return 0;
}

const char *devfs_name_of(int block, uint32_t rdev)
{
    int i;

    for (i = 0; i < DEVFS_NDEV; i++) {
        const struct devfs_entry *dev = &dev_table[i];

        if (devfs_entry_present(dev) && (((uint32_t)dev->major << 8) | dev->minor) == rdev &&
            (devfs_type_bits(dev) == 0060000u) == (block != 0)) {
            return dev->name;
        }
    }
    for (i = 0; i < reg_slot_count && !block; i++) {
        const struct devfs_entry *dev = &reg_slots[i].entry;

        if (devfs_entry_present(dev) && (((uint32_t)dev->major << 8) | dev->minor) == rdev) {
            return reg_slots[i].name;
        }
    }
    for (i = 0; i < disk_node_count; i++) {
        const struct devfs_entry *dev = &disk_nodes[i].entry;

        if (disk_node_present(i) && (((uint32_t)dev->major << 8) | dev->minor) == rdev &&
            (devfs_type_bits(dev) == 0060000u) == (block != 0)) {
            return disk_nodes[i].name;
        }
    }
    return NULL;
}

uint32_t devfs_rdev(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return 0;
    }
    dev = e->dev;
    return ((uint32_t)dev->major << 8) | (uint32_t)dev->minor;
}

uint32_t devfs_fd_ino(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return 0;
    }
    dev = e->dev;
    return DEVFS_INO(entry_index(dev));
}

int devfs_lseek_zero(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return 0;
    }
    dev = e->dev;
    return dev->lseek_zero ? 1 : 0;
}

const char *devfs_fd_name(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return "?";
    }
    dev = e->dev;
    return dev->name;
}

/** Whether `name` names a node in a directory of /dev, "input/event0" or
 * "dri/card0", and not one in /dev itself. */
static int in_subdir(const char *name)
{
    while (*name && *name != '/') {
        name++;
    }
    return *name == '/';
}

/** The entry `n`, from 0, of the slots of the registry present now whose
 * names `in_dir` accepts with `dir`: 1 with the slot's entry, 0 past the
 * last. */
static int reg_nth(int n, const char *dir, int (*in_dir)(const char *, const char *),
                   const struct reg_slot **out)
{
    int i;

    for (i = 0; i < reg_slot_count; i++) {
        if (devfs_entry_present(&reg_slots[i].entry) && in_dir(reg_slots[i].name, dir) &&
            n-- == 0) {
            *out = &reg_slots[i];
            return 1;
        }
    }
    return 0;
}

/** Whether `name` is in /dev itself; `dir` is unused. */
static int in_root(const char *name, const char *dir)
{
    (void)dir;
    return !in_subdir(name);
}

/** Whether `name` is "DIR/LEAF" for the directory `dir`, one level down. */
static int in_named_dir(const char *name, const char *dir)
{
    while (*dir && *dir == *name) {
        dir++;
        name++;
    }
    return *dir == '\0' && *name == '/' && name[1] != '\0' && !in_subdir(name + 1);
}

int devfs_dirent(int n, const char **name, uint32_t *ino)
{
    const struct reg_slot *slot;
    int i;

    if (n < 0) {
        return 0;
    }
    for (i = 0; i < DEVFS_NDEV; i++) {
        if (!devfs_entry_present(&dev_table[i]) || in_subdir(dev_table[i].name)) {
            continue;
        }
        if (n == 0) {
            *name = dev_table[i].name;
            /* The table index, as stat reports it; ttyname compares the two. */
            *ino = DEVFS_INO(i);
            return 1;
        }
        n--;
    }
    for (i = 0; i < disk_node_count; i++) {
        if (!disk_node_present(i)) {
            continue;
        }
        if (n == 0) {
            *name = disk_nodes[i].name;
            *ino = DEVFS_INO(DEVFS_NDEV + i);
            return 1;
        }
        n--;
    }
    if (reg_nth(n, "", in_root, &slot)) {
        *name = slot->name;
        *ino = DEVFS_INO(entry_index(&slot->entry));
        return 1;
    }
    return 0;
}

int devfs_subdir_dirent(const char *dir, int n, const char **name, uint32_t *ino)
{
    const struct reg_slot *slot;
    uint32_t skip = (uint32_t)k_strlen(dir) + 1;
    int i;

    if (n < 0) {
        return 0;
    }
    for (i = 0; i < DEVFS_NDEV; i++) {
        if (devfs_entry_present(&dev_table[i]) && in_named_dir(dev_table[i].name, dir) &&
            n-- == 0) {
            *name = dev_table[i].name + skip;
            *ino = DEVFS_INO(i);
            return 1;
        }
    }
    if (reg_nth(n, dir, in_named_dir, &slot)) {
        *name = slot->name + skip;
        *ino = DEVFS_INO(entry_index(&slot->entry));
        return 1;
    }
    return 0;
}

int devfs_subdir_present(int row)
{
    const struct synth_dev_dir *d = synth_dev_dir_at(row);
    const char *name;
    uint32_t ino;

    return d && (d->always || devfs_subdir_dirent(d->name, 0, &name, &ino));
}

/** Whether `name` may name a registered node: not empty, short enough for a
 * slot, and either a name of /dev itself or one name in a directory of
 * /dev's table (`synth_dev_dir_find`), with no empty part. */
static int reg_name_ok(const char *name)
{
    int len = (int)k_strlen(name);
    int slash;

    if (len == 0 || len >= DEVFS_NODE_NAME_MAX) {
        return 0;
    }
    for (slash = 0; slash < len && name[slash] != '/'; slash++) {
    }
    if (slash == len) {
        return 1;
    }
    return slash > 0 && slash < len - 1 && !in_subdir(name + slash + 1) &&
           synth_dev_dir_find(name, slash) >= 0;
}

/** Whether a node of the table, a disk or the registry already has `name`,
 * present or not, or a present character device has `major` and `minor`. */
static int reg_taken(const char *name, uint16_t major, uint16_t minor)
{
    uint32_t rdev = ((uint32_t)major << 8) | minor;
    int i;

    for (i = 0; i < DEVFS_NDEV; i++) {
        if (name_eq(dev_table[i].name, name)) {
            return 1;
        }
    }
    for (i = 0; i < disk_node_count; i++) {
        if (name_eq(disk_nodes[i].name, name)) {
            return 1;
        }
    }
    for (i = 0; i < reg_slot_count; i++) {
        if (reg_slots[i].node && name_eq(reg_slots[i].name, name)) {
            return 1;
        }
    }
    return devfs_name_of(0, rdev) != NULL;
}

int32_t devfs_node_register(struct devfs_node *node)
{
    struct reg_slot *slot = NULL;
    int i;

    if (!node->name || !node->ops || !reg_name_ok(node->name)) {
        return -EINVAL;
    }
    if (reg_taken(node->name, node->major, node->minor)) {
        return -EEXIST;
    }
    for (i = 0; i < reg_slot_count && !slot; i++) {
        if (!reg_slots[i].node && name_eq(reg_slots[i].name, node->name)) {
            slot = &reg_slots[i];
        }
    }
    if (!slot && reg_slot_count < DEVFS_NODES_MAX) {
        slot = &reg_slots[reg_slot_count++];
    }
    for (i = 0; i < reg_slot_count && !slot; i++) {
        if (!reg_slots[i].node) {
            slot = &reg_slots[i];
        }
    }
    if (!slot) {
        return -ENOSPC;
    }
    k_strncpy(slot->name, node->name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';
    slot->entry.name = slot->name;
    slot->entry.major = node->major;
    slot->entry.minor = node->minor;
    slot->entry.lseek_zero = 0;
    slot->entry.mode = (uint16_t)(node->mode & 0777u);
    slot->entry.ops = node->ops;
    slot->node = node;
    owned_drop(&slot->entry);
    return 0;
}

void devfs_node_unregister(struct devfs_node *node)
{
    int i;

    for (i = 0; i < reg_slot_count; i++) {
        if (reg_slots[i].node == node) {
            reg_slots[i].node = NULL;
            owned_drop(&reg_slots[i].entry);
        }
    }
}

uint32_t devfs_fd_mode(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return 0;
    }
    dev = e->dev;
    return devfs_type_bits(dev) | dev_mode(dev);
}

/** The owner and group of `dev` into `*uid` and `*gid`. */
static void dev_owner(const struct devfs_entry *dev, uint32_t *uid, uint32_t *gid)
{
    uint32_t flags = kirq_save();
    const struct devfs_owned *o = owned_of(dev);

    *uid = o ? o->uid : 0;
    *gid = o ? o->gid : 0;
    kirq_restore(flags);
}

int devfs_owner(const char *name, uint32_t *uid, uint32_t *gid)
{
    const struct devfs_entry *dev = name[0] != '\0' ? devfs_find(name) : NULL;

    *uid = 0;
    *gid = 0;
    if (name[0] == '\0') {
        return 0;
    }
    if (!dev) {
        return -1;
    }
    dev_owner(dev, uid, gid);
    return 0;
}

void devfs_fd_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid)
{
    *uid = 0;
    *gid = 0;
    if (e && e->type == FD_TYPE_DEVICE && e->dev) {
        dev_owner(e->dev, uid, gid);
    }
}

int32_t devfs_setattr(const char *name, uint32_t mode, uint32_t uid, uint32_t gid)
{
    const struct devfs_entry *dev = name[0] != '\0' ? devfs_find(name) : NULL;
    struct devfs_owned *o = NULL;
    uint32_t flags;
    int i;

    if (!dev) {
        return -ENOENT;
    }
    flags = kirq_save();
    for (i = 0; i < DEVFS_OWNED_MAX && !o; i++) {
        if (owned[i].dev == dev) {
            o = &owned[i];
        }
    }
    for (i = 0; i < DEVFS_OWNED_MAX && !o; i++) {
        if (!owned[i].dev) {
            o = &owned[i];
            o->dev = dev;
            o->uid = 0;
            o->gid = 0;
            o->mode = dev->mode;
        }
    }
    if (o) {
        if (mode != (uint32_t)-1) {
            o->mode = (uint16_t)(mode & 0777u);
        }
        if (uid != (uint32_t)-1) {
            o->uid = uid;
        }
        if (gid != (uint32_t)-1) {
            o->gid = gid;
        }
    }
    kirq_restore(flags);
    return o ? 0 : -ENOSPC;
}

void devfs_disk_withdraw(void)
{
    int i;

    for (i = 0; i < disk_count(); i++) {
        struct disk *d = disk_at(i);

        /* A released slot is still counted and has no record. */
        if (d) {
            d->whole.present = 0;
        }
    }
}

/**
 * The whole node is published whatever the table said, because a medium with
 * no table at all is a volume in its own right and is mounted through that
 * node; one with a table is read through it as well, which is how a partition
 * table is inspected without mounting anything. The partition windows are
 * published apart from it (`disk_set_table`).
 */
void devfs_disk_publish(struct disk *d)
{
    if (!d || d->dev->sector_count == 0) {
        klog(KLOG_DEBUG, "devfs", "no whole-disk window to publish");
        return;
    }
    block_partition_init(&d->whole.part, d->dev, 0, d->dev->sector_count);
    d->whole.present = 1;
}

/**
 * The log seeks like a file whose size is what it holds now, so SEEK_END and
 * a plain `cat` both behave. Its size is 32 bits, which a ring of kilobytes
 * does not outgrow.
 */
int devfs_fd_log_size(struct fd_entry *e, uint32_t *size)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return 0;
    }
    dev = e->dev;
    if (dev->ops == &bootlog_ops) {
        if (size) {
            *size = klog_len();
        }
        return 1;
    }
    return 0;
}

int devfs_fd_moves_offset(struct fd_entry *e)
{
    if (devfs_fd_is_raw(e) || devfs_fd_log_size(e, NULL)) {
        return 1;
    }
    return e && e->type == FD_TYPE_DEVICE && e->dev && e->dev->ops == &screenlog_ops;
}

/**
 * The window is bounded below 2 GiB so the 32-bit lseek can name every byte
 * of it; the wider windows are what _llseek is for. The image tools make the
 * partition tens of mebibytes, so one past the bound is a misbuilt disk, and
 * the device stays absent with a warning.
 */
void devfs_bootpart_publish(struct block_device *backing, uint32_t start_lba, uint32_t sector_count)
{
    if (sector_count == 0 || sector_count >= (0x80000000u / BLOCK_SECTOR_SIZE)) {
        klog(KLOG_WARN, "devfs", "boot partition too large; /dev/bootpart absent");
        return;
    }
    block_partition_init(&bootpart.part, backing, start_lba, sector_count);
    bootpart.present = 1;
}

/**
 * Discovery publishes the device before any root is chosen, judging entry 0
 * against the whole partition table. One fact is only knowable after the
 * mount: which entry has the root. Entry 0 can be FAT-typed and still be the
 * entry the root was found in, and a boot partition over the root is not one
 * this machine should offer. The callers have different reasons, so the
 * caller logs its own.
 */
int devfs_bootpart_withdraw(void)
{
    if (!bootpart.present) {
        return 0;
    }
    bootpart.present = 0;
    return 1;
}

/**
 * The window names a backing device, and a driver that is initialised again
 * clears the state that device reads through. Anything that resets a
 * controller takes the window down first and publishes it again afterwards.
 */
void devfs_grubcore_withdraw(void)
{
    grubcore.present = 0;
}

/**
 * Sector 0 is outside the window because that sector has the partition
 * table and a disk writes whole sectors, so there is no way to touch the
 * eight bytes of boot code that point at the core image without rewriting
 * the table beside them. A device too small for the gap has no window, and
 * a warning says so.
 */
void devfs_grubcore_publish(struct block_device *backing)
{
    if (!backing || backing->sector_count < (uint64_t)MBR_GAP_END_LBA) {
        klog(KLOG_WARN, "devfs", "no room for the boot loader gap; /dev/grubcore absent");
        return;
    }
    block_partition_init(&grubcore.part, backing, MBR_GAP_FIRST_LBA,
                         MBR_GAP_END_LBA - MBR_GAP_FIRST_LBA);
    grubcore.present = 1;
}

/**
 * `t` is from the primary header or, when that one failed its checks, the
 * backup. The partition is found by `part_bios_boot`, and one that runs past
 * the end of `backing` has no window. Sector 0 is outside the window, so the
 * protective MBR's pointer to the core's first sector stays as the disk was
 * built.
 */
void devfs_grubcore_publish_gpt(struct block_device *backing, const struct part_table *t)
{
    uint64_t first, count;
    int number;

    if (!backing || part_bios_boot(t, &first, &count, &number) != 0) {
        klog(KLOG_INFO, "devfs",
             "no one BIOS boot partition clear of the GUID table; /dev/grubcore absent");
        return;
    }
    if (count > backing->sector_count || first > backing->sector_count - count) {
        klog(KLOG_WARN, "devfs", "the BIOS boot partition is past the disk; /dev/grubcore absent");
        return;
    }
    block_partition_init(&grubcore.part, backing, first, count);
    grubcore.present = 1;
    klog(KLOG_INFO, "devfs", "/dev/grubcore: BIOS boot partition %d, %lu sectors at LBA %lu",
         number, (unsigned long)count, (unsigned long)first);
}

/**
 * HDIO_GETGEO: the `struct hd_geometry` of a raw window, 255 heads and 63
 * sectors a track, as Linux makes up for a disk without one, the cylinders
 * those give, cut to 16 bits as Linux cuts them, and the window's first
 * sector, 0 for a whole disk. BusyBox's `fdisk -l` reads the start to tell a
 * whole disk, whose name here ends in a digit, from a partition. 0,
 * -ENOTTY for a device that is no raw window or one not published, or
 * -EFAULT.
 */
static int32_t dev_getgeo(fd_entry_t *e, uintptr_t arg)
{
    struct {
        uint8_t heads;
        uint8_t sectors;
        uint16_t cylinders;
        unsigned long start;
    } geo; /* Linux's struct hd_geometry */
    struct disk_window *w = e->dev ? raw_window_of(e->dev) : NULL;
    uint64_t sectors;

    if (!w || !w->present) {
        return -ENOTTY;
    }
    sectors = w->part.device.sector_count;
    /* The padding after `cylinders`, 4 bytes on x86-64, is copied out too. */
    k_memset(&geo, 0, sizeof(geo));
    geo.heads = 255;
    geo.sectors = 63;
    geo.cylinders =
        (uint16_t)((sectors > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)sectors) / (255u * 63u));
    geo.start = (unsigned long)w->part.start_lba;
    return copy_to_user((void __user *)arg, &geo, sizeof(geo)) != 0 ? -EFAULT : 0;
}

/** BLKROGET, BLKRAGET and BLKBSZGET of a disk or loop node: read-only only
 * for a loop device attached so, 256 sectors of read-ahead and 4096-byte
 * blocks; an int each, but the read-ahead, which Linux gives as a long. 0,
 * -ENOTTY for a device with no size or /dev/mem, or -EFAULT. */
static int32_t dev_block_info(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    uint64_t size;
    const block_device_t *loop =
        e->dev && e->dev->ops == &loop_ops ? loop_attached_at(devfs_loop_index(e->dev)) : 0;
    int32_t v = request == 0x125Eu ? (loop && (loop->flags & BLOCK_READ_ONLY) ? 1 : 0) : 4096;
    long readahead = 256;

    if (!devfs_fd_raw_size(e, &size) || e->dev->ops == &mem_ops) {
        return -ENOTTY;
    }
    if (request == 0x1263u) {
        return copy_to_user((void __user *)arg, &readahead, sizeof(readahead)) != 0 ? -EFAULT : 0;
    }
    return copy_to_user((void __user *)arg, &v, sizeof(v)) != 0 ? -EFAULT : 0;
}

/** The sector size of a raw window or an attached loop device, 512 bytes,
 * as an int at `arg`: 0, -ENOTTY for another device, or -EFAULT. */
static int32_t dev_sector_size(fd_entry_t *e, uintptr_t arg)
{
    int32_t sector = 512;
    struct disk_window *w = e->dev ? raw_window_of(e->dev) : NULL;
    int loop = e->dev && e->dev->ops == &loop_ops && loop_attached_at(devfs_loop_index(e->dev));

    if (!loop && (!w || !w->present)) {
        return -ENOTTY;
    }
    return copy_to_user((void __user *)arg, &sector, sizeof(sector)) != 0 ? -EFAULT : 0;
}

int devfs_fd_is_raw(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return 0;
    }
    dev = e->dev;
    return dev->ops == &bootpart_ops || dev->ops == &grubcore_ops || dev->ops == &diskwin_ops ||
           dev->ops == &mem_ops || dev->ops == &loop_ops;
}

int devfs_fd_is_mem(struct fd_entry *e)
{
    return e && e->type == FD_TYPE_DEVICE && e->dev && e->dev->ops == &mem_ops;
}

int devfs_fd_raw_size(struct fd_entry *e, uint64_t *size)
{
    const struct devfs_entry *dev;
    struct disk_window *w;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return 0;
    }
    dev = e->dev;
    if (dev->ops == &mem_ops) {
        if (size) {
            *size = DEVMEM_SIZE;
        }
        return 1;
    }
    if (dev->ops == &loop_ops) {
        const block_device_t *loop = loop_attached_at(devfs_loop_index(dev));

        if (loop && size) {
            *size = loop->sector_count * BLOCK_SECTOR_SIZE;
        }
        return loop != 0;
    }
    w = raw_window_of(dev);
    if (!w || !w->present) {
        return 0;
    }
    if (size) {
        *size = w->part.device.sector_count * BLOCK_SECTOR_SIZE;
    }
    return 1;
}
