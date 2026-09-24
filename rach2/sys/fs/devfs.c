/*
 * devfs.c: a synthetic /dev built on the file_ops dispatch.
 *
 * Each device is a tiny file_ops vtable; a registry binds a leaf name to its
 * ops and a {major,minor}. open("/dev/<name>") allocates a descriptor and
 * points its ops and dev at the registry entry (FD_TYPE_DEVICE), after which
 * the generic read/write/close paths dispatch through the vtable, so the
 * device code lives nowhere in the syscall layer. The namespace is virtual:
 * /dev is not stored in KRFFS; stat and getdents synthesize from this table.
 */

#include "../klog.h"
#include "devfs.h"

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
#include "../io/poll.h"
#include "../tty/pty.h"
#include "../block/block.h"
#include "../block/gpt.h"
#include "../block/usbdisk.h"
#include "../block/recovery.h"
#include "../block/disk.h"
#include "../serial.h"

/* Linux O_* bits devfs_open honours (the rest are irrelevant to a char
 * device). */
#define DEVFS_O_CLOEXEC   02000000
#define DEVFS_O_NONBLOCK  04000
#define DEVFS_O_CREAT     0100
#define DEVFS_O_EXCL      0200
#define DEVFS_O_DIRECTORY 0200000

/*
 * The backings. /dev/null and /dev/zero sink writes; /dev/full fails them with
 * -ENOSPC. Reads come from a small kernel buffer copied out in chunks so an
 * arbitrarily large count never needs an arbitrarily large stack.
 */

static int32_t dev_eof_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return 0; /* /dev/null: always at end of file */
}

static int32_t dev_sink_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    return (int32_t)count; /* accept and discard */
}

static int32_t dev_fill_read(fd_entry_t *e, char __user *buf, uint32_t count, int random)
{
    char tmp[256];
    uint32_t done = 0;

    (void)e;
    if (!random) {
        k_memset(tmp, 0, sizeof(tmp));
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

static int32_t dev_zero_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return dev_fill_read(e, buf, count, 0); /* /dev/zero and /dev/full */
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

static int32_t dev_full_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -ENOSPC; /* /dev/full: the disk is always full */
}

/**
 * Report the requested read and write readiness bits without registering a
 * wait queue. Raw disk transfers may still sleep while completing I/O.
 */
static short dev_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    (void)e;
    if (wq) {
        *wq = NULL;
    }
    return (short)(events & (POLLIN | POLLOUT));
}

static int32_t dev_disk_rescan(fd_entry_t *e);

/*
 * The size of a raw window, which is what a checker asks a block device
 * before it reads it: `BLKGETSIZE64` is `_IOR(0x12, 114, size_t)`, which is
 * 0x80041272 with musl's 32-bit `size_t` and 0x80081272 from a 64-bit
 * header, and both take eight bytes; `BLKGETSIZE` (0x1260) is the sector
 * count in an unsigned long. `BLKRRPART` (0x125F) rereads a whole disk's
 * partition table. Everything else is not a terminal.
 */
static int32_t dev_ioctl(fd_entry_t *e, uint32_t request, uint32_t arg)
{
    uint64_t size;

    if (request == 0x80041272u || request == 0x80081272u) {
        if (!devfs_fd_raw_size(e, &size)) {
            return -ENOTTY;
        }
        return copy_to_user((void __user *)arg, &size, sizeof(size)) != 0 ? -EFAULT : 0;
    }
    if (request == 0x125Fu) {
        return dev_disk_rescan(e);
    }
    if (request == 0x1260u) {
        uint32_t sectors;

        if (!devfs_fd_raw_size(e, &size)) {
            return -ENOTTY;
        }
        if ((size >> 9) > 0xFFFFFFFFu) {
            return -EFBIG;
        }
        sectors = (uint32_t)(size >> 9);
        return copy_to_user((void __user *)arg, &sectors, sizeof(sectors)) != 0 ? -EFAULT : 0;
    }
    return -ENOTTY;
}

/*
 * One entry of the registry: a leaf name under /dev, the {major,minor} stat
 * reports for it, and the vtable an open binds to a descriptor. The table
 * itself and what each field means are below, beside it.
 */
struct devfs_entry {
    const char *name;
    uint16_t major;
    uint16_t minor;
    uint8_t lseek_zero; /* mem devices seek-to-zero; the rest are not seekable */
    uint16_t mode;
    const struct file_ops *ops;
};

/*
 * /dev/bootpart: the GRUB disk's FAT boot partition as a bounded raw window,
 * published by the disk mount when the MBR qualifies (devfs_bootpart_publish)
 * and absent otherwise. I/O is whole 512-byte sectors at sector-aligned
 * offsets, staged through a kernel bounce buffer because the AHCI driver
 * programs the buffer's physical address into its PRDT and must never see a
 * user pointer. Writes are write-through: both disk drivers flush the drive
 * cache after every write, so a returned write is on the platter.
 */

/*
 * The static staging buffer, and the number of sectors one driver call has.
 *
 * A user pointer can never go to a driver: AHCI programs the buffer's physical
 * address into its PRDT, so the bytes must be somewhere the kernel knows the
 * physical address of. This array is in `.bss`, which the linker places below
 * the 32 MiB identity-mapped limit, so its virtual address is its physical one
 * and it is contiguous.
 *
 * One driver call moves up to RAW_BATCH_SECTORS, a mebibyte, and a driver
 * that takes less splits it. The size is what makes a whole-disk reflash
 * finishable at all: one sector per call would make a 500 MiB write about a
 * million driver calls, and as many cache flushes, since both drivers flush
 * the drive's cache after every write command.
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

/* How many driver calls the raw windows have made. Batching is otherwise
 * invisible from outside: a transfer of any size returns the same answer
 * whether it took one command or two hundred, and only the time differs. This
 * is reported by /proc/disk so a test can require the batching, not the
 * result, which is the difference between a 500 MiB reflash taking hundreds
 * of driver calls and a million. */
static uint64_t raw_calls;

/*
 * Who has the static staging buffer. A window over a device that sleeps
 * holds its buffer across the sleep, and a second process filling the same
 * buffer under the sleeper would hand it the wrong sectors; so the static
 * buffer is taken first and given back after the copy, and a second process
 * that also found the heap empty sleeps until then.
 */
static int raw_batch_holder = -1;
static wait_queue_t raw_batch_wq;

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
 * `raw_batch_put` is given back.
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

static struct disk_window bootpart;

/*
 * /dev/grubcore: the post-MBR gap, LBA 1 through 2047, as a bounded raw
 * window. GRUB's core image lives there, outside every partition, so updating
 * the boot loader needs a device and /dev/bootpart cannot be it: that one is
 * relative to partition 1 and the gap is before it. The range is a constant of
 * the layout instead of anything the partition table says, so a damaged table
 * cannot widen it. Sector 0 is deliberately outside: it has the partition
 * table, and a disk writes whole sectors.
 */
static struct disk_window grubcore;

/**
 * Read whole sectors from a bounded raw window.
 *
 * Offsets and counts must be whole sectors: a partial sector has no meaning to
 * a disk, and rounding one would either read a neighbour's bytes or write
 * them. The end is handled the way a regular file's is, with a short read,
 * not an error, because a reader walking a window to its end is
 * ordinary. Sectors are staged through a kernel buffer because the AHCI driver
 * programs the buffer's physical address into its PRDT and must never be given
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
        return rc; /* the request's own errno: EIO, or what the device said */
    }
    e->ofd->raw_offset = off + (uint64_t)done * BLOCK_SECTOR_SIZE;
    return (int32_t)(done * BLOCK_SECTOR_SIZE);
}

/**
 * Write whole sectors to a bounded raw window.
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
    if (!current_proc || current_proc->euid != 0) {
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

static int32_t dev_bootpart_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return raw_window_read(&bootpart, e, buf, count);
}

#define DISK_NODE_WINDOWS (DISK_PARTS + 1)
#define DISK_MINOR_BASE   64

struct disk_node {
    struct devfs_entry entry; /* first, so an entry pointer is the node */
    char name[16];
    struct disk *disk;
    int part; /* 0 for the whole disk, else the 1-based partition */
};

static struct disk_node disk_nodes[DISK_MAX * DISK_NODE_WINDOWS];
static int disk_node_count;

static const struct file_ops diskwin_ops;

static const struct disk_node *disk_node_of(const struct devfs_entry *dev)
{
    if (dev->ops != &diskwin_ops) {
        return 0;
    }
    return (const struct disk_node *)dev;
}

/* The node behind a descriptor open on a disk window, or NULL for a
   descriptor open on anything else. `dev_ioctl` is reached by every device,
   not only the windows, so the entry's operations are what says whether its
   record is a node: a plain `devfs_entry` read as one reads past the table
   it is in. */
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
 * Read a disk window at its descriptor's byte offset.
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
 * partition is mounted or another descriptor is open on the disk.
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
 * Write whole sectors anywhere on a disk, in a recovery build only.
 *
 * This is the one place in the kernel that can destroy the machine outright,
 * so which kernel it is compiled into is decided when the kernel is built and
 * not when it boots. An everyday build compiles the refusal below and has no
 * path around it; a `-DRACH2_RECOVERY` build compiles the body. Both have a
 * banner naming which they are, and `check_recovery.sh` reads it off the
 * artifact in both directions.
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
 * at whatever offset it lives, and a block device node on Linux answers
 * that. So the covering sectors are read into the staging buffer and the
 * bytes asked for are copied out of it, one batch at a time; the end is a
 * short read, as a regular file's is.
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

static int32_t dev_bootpart_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return raw_window_write(&bootpart, e, buf, count);
}

static int32_t dev_grubcore_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return raw_window_read(&grubcore, e, buf, count);
}

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

static void dev_grubcore_close(fd_entry_t *e)
{
    grubcore_held = 0;
    raw_close(e);
}

static void dev_bootpart_close(fd_entry_t *e)
{
    raw_close(e);
}

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
 * A loop device is reached through a mount and not through its node: the node
 * is what `mount(2)` names as the source, and reading or writing it directly
 * would be a second way to the same bytes, which the image under a mounted
 * file system may not have. The control node takes one line per write:
 * `attach N /path`, `attach - /path` for the first free device, `detach N`;
 * `ro` after `attach` attaches read-only.
 */
static int32_t loop_node_read(fd_entry_t *entry, char __user *buffer, uint32_t count)
{
    (void)entry;
    (void)buffer;
    (void)count;
    return -EPERM;
}

static int32_t loop_node_write(fd_entry_t *entry, const char __user *buffer, uint32_t count)
{
    (void)entry;
    (void)buffer;
    (void)count;
    return -EPERM;
}

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

static const struct file_ops loop_ops = {loop_node_read, loop_node_write, dev_poll, dev_ioctl,
                                         NULL};

/* `/dev/framefail`: a count written to it arms the test switch in the
 * frame allocator; EPERM unless `rach2.framefail` is on the boot line. */
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

static const struct file_ops framefail_ops = {dev_eof_read, framefail_write, dev_poll, dev_ioctl,
                                              NULL};
static const struct file_ops loopctl_ops = {dev_eof_read, loopctl_write, dev_poll, dev_ioctl, NULL};

static const struct file_ops null_ops = {dev_eof_read, dev_sink_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops zero_ops = {dev_zero_read, dev_sink_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops full_ops = {dev_zero_read, dev_full_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops rand_ops = {dev_rand_read, dev_sink_write, dev_poll, dev_ioctl, NULL};
/*
 * /dev/bootlog: what this machine printed while it was starting.
 *
 * It exists so a question about a boot can be asked after that boot, over the
 * network, by somebody who arranged nothing in advance. The alternatives all
 * demand foresight: the screen scrolls the log past in under a second, the
 * netconsole reaches only a capture that was already running when the power
 * came on, and holding the screen helps only a person standing at the machine.
 *
 * What it holds is the kernel's records, from the boot and from whatever the
 * machine has said since, and not the debug shell's own output.
 */
/**
 * Copy from the log's ring into a reader's buffer.
 *
 * The descriptor's offset counts from the oldest record the ring still holds,
 * which is what `klog_read` takes. Records may be evicted between chunk
 * copies, including within one read. The returned bytes are not a snapshot.
 * A fault after some bytes were copied is a short read: those bytes are
 * reported and the offset moves past them.
 */
static int32_t dev_bootlog_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    char chunk[256];
    uint32_t done = 0;

    while (done < count) {
        uint32_t want = count - done;
        uint32_t n;

        if (want > sizeof(chunk)) {
            want = (uint32_t)sizeof(chunk);
        }
        n = klog_read(chunk, want, (uint32_t)e->ofd->offset + done);
        if (n == 0) {
            break; /* the end of the log, the regular-file convention */
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

/*
 * /dev/screenlog: how much of the machine's own record the screen shows.
 *
 * Reading gives the level as one digit and a newline; writing one digit sets
 * it. The ring, the serial port and netconsole are not affected, so nothing a
 * later reader wants is lost by turning the screen down. It exists because the
 * screen is shared: a program that is talking to a person, `rach2-install`
 * above all, has no other way to stop the kernel's routine notes arriving in
 * the middle of its questions.
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
    /* The whole write is taken, so a caller that wrote "4\n" is not asked to
       write the newline again. */
    return (int32_t)count;
}

static const struct file_ops screenlog_ops = {dev_screenlog_read, dev_screenlog_write, dev_poll,
                                              dev_ioctl, NULL};

static const struct file_ops grubcore_ops = {dev_grubcore_read, dev_grubcore_write, dev_poll,
                                             dev_ioctl, dev_grubcore_close};
static const struct file_ops bootpart_ops = {dev_bootpart_read, dev_bootpart_write, dev_poll,
                                             dev_ioctl, dev_bootpart_close};
static const struct file_ops bootlog_ops = {dev_bootlog_read, dev_sink_write, dev_poll, dev_ioctl,
                                            NULL};
static const struct file_ops diskwin_ops = {dev_disk_read, dev_disk_write, dev_poll, dev_ioctl,
                                            dev_disk_close};

/* Whether `dev` is a raw window a write may reach: the whole disks and the
 * two boot windows in every build, the partitions in the recovery build. */
static int raw_write_window(const struct devfs_entry *dev)
{
    const struct disk_node *node = disk_node_of(dev);

    if (node) {
        return node->part == 0 ? 1 : recovery_publishes_disk();
    }
    return dev->ops == &bootpart_ops || dev->ops == &grubcore_ops;
}

/* The raw window of `dev`, a disk's or one of the two boot windows, or null. */
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

/* The disk a raw window is on, for its writer reservation, or null. */
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
 * The registry. The mem devices have the real Linux numbers (major 1) and the
 * terminals theirs (major 5), so stat reports a faithful rdev; the rest are in
 * the local block, major 240. A device with nothing to release has a NULL
 * close, and the descriptor's `dev` points at the registry entry itself,
 * which fstat reads for the rdev. `mode` is the nine-bit permission stat
 * reports and open enforces (the entries' owner is root:root): the mem
 * devices and the terminals are 0666, the Linux modes, and every other entry
 * is root's. `bootpart`, `grubcore`, the loop devices and the disk nodes are
 * present only at times, which is why enumeration goes through
 * `devfs_dirent` and skips absent entries instead of stopping at the first
 * one: a machine that has a later one and not an earlier one would otherwise
 * have a device it could open and could not list.
 */
/* ops == NULL marks a terminal entry, which devfs_open handles itself:
 * console and tty yield an FD_TYPE_CONSOLE descriptor (tty a pty when the
 * caller's session is on one), ptmx a new pty pair. bootpart's major is 240,
 * the local/experimental block, minor 0. */
static const struct devfs_entry dev_table[] = {{"null", 1, 3, 1, 0666, &null_ops},
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
                                               {"loop0", 240, 32, 0, 0400, &loop_ops},
                                               {"loop1", 240, 33, 0, 0400, &loop_ops},
                                               {"loop2", 240, 34, 0, 0400, &loop_ops},
                                               {"loop3", 240, 35, 0, 0400, &loop_ops},
                                               {"loop-control", 240, 36, 0, 0600, &loopctl_ops},
                                               {"framefail", 240, 37, 0, 0600, &framefail_ops},
                                               {"screenlog", 240, 38, 0, 0600, &screenlog_ops}};

#define DEVFS_NDEV ((int)(sizeof(dev_table) / sizeof(dev_table[0])))

/** The loop device an entry names, or -1 for an entry that is not one. */
static int devfs_loop_index(const struct devfs_entry *dev)
{
    if (dev->ops != &loop_ops) {
        return -1;
    }
    return (int)(dev->minor - 32);
}

/** Whether table entry `i` exists right now: bootpart, grubcore and the loop
 * devices come and go. */
static int devfs_entry_present(int i)
{
    if (dev_table[i].ops == &loop_ops) {
        return loop_device_at(devfs_loop_index(&dev_table[i])) != 0;
    }
    if (dev_table[i].ops == &bootpart_ops) {
        return bootpart.present;
    }
    if (dev_table[i].ops == &grubcore_ops) {
        return grubcore.present;
    }
    return 1;
}

/**
 * The type bits `stat` reports for an entry.
 *
 * The disks' nodes are block devices and everything else here is a
 * character device. The difference is not decoration: `blkid`, `findfs`
 * and `mount LABEL=` walk `/dev` and look at nothing that is not a block
 * device, so a volume reported as a character device cannot be found by
 * its label.
 */
static uint32_t devfs_type_bits(const struct devfs_entry *dev)
{
    return dev->ops == &diskwin_ops ? 0060000u : 0020000u; /* S_IFBLK, S_IFCHR */
}

/* Stable inode namespace for /dev: the directory itself, then one per
 * device. */
#define DEVFS_INO_DIR  2u
#define DEVFS_INO_BASE 16u
#define DEVFS_INO(idx) (DEVFS_INO_BASE + (uint32_t)(idx))

/* Equal NUL-terminated strings? (The kernel has no strcmp.) */
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

/* The index `stat` and the listing agree on: the table's entries first,
 * then the disk nodes in the order the disks were registered. */
static int entry_index(const struct devfs_entry *dev)
{
    const struct disk_node *node = disk_node_of(dev);

    if (node) {
        return DEVFS_NDEV + (int)(node - disk_nodes);
    }
    return (int)(dev - dev_table);
}

static const struct devfs_entry *devfs_find(const char *name)
{
    int i;
    for (i = 0; i < DEVFS_NDEV; i++) {
        if (name_eq(dev_table[i].name, name)) {
            return devfs_entry_present(i) ? &dev_table[i] : NULL;
        }
    }
    for (i = 0; i < disk_node_count; i++) {
        if (name_eq(disk_nodes[i].name, name)) {
            return disk_node_present(i) ? &disk_nodes[i].entry : NULL;
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

/**
 * Create registry nodes for a disk and its partition slots.
 *
 * `/dev/diskN` names the whole disk; `/dev/diskNpM` names partition M.
 * Partition nodes are published after a table scan. USB discovery also
 * publishes the whole-disk node; disk diagnostics publish whole-disk
 * windows for registered disks. A table scan updates the partition names.
 *
 * Disk 0 uses minor 1 for the whole disk and minors 4 through 7 for its
 * first four partitions. Other nodes use minor 64 + 16N + M, with M zero
 * for the whole disk.
 */
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

/* The range of a raw window on its backing disk, for the claim rules. */
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
        block_device_t *loop = loop_device_at(devfs_loop_index(dev));

        if (!loop) {
            return -ENOENT;
        }
        /* A file has no sectors on a disk to claim: the device itself is the
         * identity, and only one mount can be placed on it. */
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
        /* The whole disk is a mount's source when it holds one volume and
         * no table, as a stick formatted without one does; the claim rules
         * keep a whole-disk mount and a partition's from overlapping. */
        w = disk_node_window(node);
    }
    if (!w) {
        return -EINVAL; /* not a partition window */
    }
    if (!w->present) {
        return -ENOENT; /* devfs_find lists only published nodes */
    }
    *view = &w->part.device;
    *backing = block_physical(w->part.backing);
    *first = w->part.start_lba;
    *count = w->part.device.sector_count;
    return 0;
}

/**
 * Open a /dev leaf by name and return its descriptor.
 *
 * `name` is the leaf under /dev, `flags` the Linux O_* set. Most entries bind
 * an fd to the registry vtable (FD_TYPE_DEVICE); three are special. /dev/ptmx
 * allocates a fresh pty pair per open. /dev/console and /dev/tty map to the
 * kernel console (FD_TYPE_CONSOLE), except that /dev/tty
 * resolves to the caller's controlling terminal, which may be a pty rather than
 * the console.
 *
 * /dev/tty follows POSIX: the console when the caller is in the console's
 * session, else the pty slave that owns the caller's session, else ENXIO,
 * which is also what dropbear's probe after setsid expects. A job-control
 * shell over a pty opens /dev/tty to set its foreground process group, and
 * given the console instead it would never see itself in the foreground and
 * would stop on SIGTTIN.
 *
 * A write-capable open of a raw window, the whole disk, the two boot windows
 * or, in the recovery build, a partition, takes the disk's one writer
 * reservation, and is refused with EBUSY while another writer has it or a
 * mounted volume claims the window's range. A second open of /dev/grubcore is
 * refused with EBUSY too.
 *
 * Reject `O_CREAT|O_EXCL` for an existing leaf before allocating a descriptor
 * or a pty pair.
 *
 * @return the new descriptor, or a negative errno.
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
        /* ctty is the console -> fall through to the FD_TYPE_CONSOLE path
         * below. */
    }

    if (dev->ops == &grubcore_ops && grubcore_held) {
        return -EBUSY;
    }
    if ((flags & 3) != 0 && raw_write_window(dev)) {
        struct disk *d = window_disk(dev);
        block_device_t *backing;
        uint64_t first, last;

        if (d && d->writer_held) {
            return -EBUSY;
        }
        /* A mounted volume's sectors are nobody else's to write. */
        if (window_range(dev, &backing, &first, &last) == 0 && claimed(backing, first, last)) {
            return -EBUSY;
        }
    }

    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        return -EMFILE;
    }

    e = &current_proc->fds.entries[fd];
    e->node = NULL;
    e->sock = NULL;
    e->ofd->offset = 0;
    if (dev->ops) {
        e->type = FD_TYPE_DEVICE;
        e->ops = dev->ops;
        e->dev = dev;
    } else {
        /* /dev/console and /dev/tty reuse the kernel console (enum
         * dispatch). */
        e->type = FD_TYPE_CONSOLE;
        e->ops = NULL;
        e->priv = NULL;
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

    return fd;
}

/**
 * Synthesize stat fields for /dev or a device leaf.
 *
 * An empty `name` identifies `/dev`; otherwise it identifies a device leaf.
 * Disk nodes report block-device mode; other leaves report character-device
 * mode. Each out pointer is optional. rdev packs the entry's {major,minor}
 * the way the Linux mem devices number themselves.
 *
 * @return 0 on success, -1 if the named leaf does not exist.
 */
int devfs_stat(const char *name, uint32_t *mode, uint32_t *rdev, uint32_t *ino)
{
    const struct devfs_entry *dev;

    if (name[0] == '\0') {
        /* /dev itself: a directory. */
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
        *mode = devfs_type_bits(dev) | (uint32_t)dev->mode;
    }
    if (rdev) {
        *rdev = ((uint32_t)dev->major << 8) | (uint32_t)dev->minor;
    }
    if (ino) {
        *ino = DEVFS_INO(entry_index(dev));
    }
    return 0;
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

int devfs_dirent(int n, const char **name, uint32_t *ino)
{
    int i;

    if (n < 0) {
        return 0;
    }
    for (i = 0; i < DEVFS_NDEV; i++) {
        if (!devfs_entry_present(i)) {
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
    return 0;
}

uint32_t devfs_fd_mode(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return 0;
    }
    dev = e->dev;
    return devfs_type_bits(dev) | (uint32_t)dev->mode;
}

/**
 * Withdraw every registered disk's whole-disk window.
 *
 * A controller reset invalidates the device state these windows reference.
 * Unpublished windows remain unpublished. Partition windows are unchanged.
 */
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
 * Publish disk `d`'s whole-disk window as `/dev/diskN`.
 *
 * `disk_set_table` publishes partition windows separately.
 *
 * The whole node is published whatever the table said, because a medium with
 * no table at all is a volume in its own right and is mounted through that
 * node; one with a table is read through it as well, which is how a partition
 * table is inspected without mounting anything.
 */
void devfs_disk_publish(struct disk *d, uint64_t window_sectors)
{
    if (!d || window_sectors == 0) {
        klog(KLOG_DEBUG, "devfs", "no whole-disk window to publish");
        return;
    }
    block_partition_init(&d->whole.part, d->dev, 0, window_sectors);
    d->whole.present = 1;
}

/**
 * The size a seek on `/dev/bootlog` measures against.
 *
 * The log seeks like a file whose size is what it currently holds, so SEEK_END
 * and a plain `cat` both behave. It is not a raw window: it reads through the
 * ordinary 32-bit offset, and a ring of kilobytes has no use for a wider one.
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

/**
 * Publish /dev/bootpart over the boot partition.
 *
 * The window is bounded below 2 GiB so the legacy 32-bit lseek can name every
 * byte of it; the wider windows are what _llseek exists for. The partition is
 * 16 MiB on a test image and 64 MiB on the sealed disk, so anything past the
 * bound is a misbuilt disk and the device stays absent.
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
 * Withdraw /dev/bootpart.
 *
 * Discovery publishes the device before any root is chosen, judging entry 0
 * against the whole partition table. One fact is only knowable after the mount:
 * which entry has the root. Entry 0 can be FAT-typed and still be the entry
 * the root was found in, and a boot partition sitting over the root is not one
 * this machine should offer. A no-op when the device was never published.
 */
void devfs_bootpart_withdraw(void)
{
    if (bootpart.present) {
        klog(KLOG_WARN, "devfs", "entry 0 has the root; /dev/bootpart withdrawn");
        bootpart.present = 0;
    }
}

/**
 * Withdraw /dev/grubcore.
 *
 * The window names a backing device, and a driver that is re-initialised
 * clears the state that device reads through. Anything that resets a
 * controller takes the window down first and republishes afterwards.
 * A no-op when the device was never published.
 */
void devfs_grubcore_withdraw(void)
{
    grubcore.present = 0;
}

/**
 * Publish /dev/grubcore over the post-MBR gap.
 *
 * The range is the layout's, not the partition table's: LBA 1 through 2047,
 * fixed, so nothing a damaged table says can widen it. Sector 0 is outside it
 * because that sector has the partition table and a disk writes whole
 * sectors, so there is no way to touch the eight bytes of boot code that point
 * at the core image without rewriting the table beside them.
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
 * Return whether `e` identifies a boot window, whole-disk window or partition
 * window, regardless of whether it is currently published.
 */
int devfs_fd_is_raw(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return 0;
    }
    dev = e->dev;
    return dev->ops == &bootpart_ops || dev->ops == &grubcore_ops || dev->ops == &diskwin_ops;
}

int devfs_fd_raw_size(struct fd_entry *e, uint64_t *size)
{
    const struct devfs_entry *dev;
    struct disk_window *w;

    if (!e || e->type != FD_TYPE_DEVICE || !e->dev) {
        return 0;
    }
    dev = e->dev;
    w = raw_window_of(dev);
    if (!w || !w->present) {
        return 0;
    }
    if (size) {
        *size = w->part.device.sector_count * BLOCK_SECTOR_SIZE;
    }
    return 1;
}
