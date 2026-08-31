/*
 * devfs.c: a synthetic /dev built on the file_ops dispatch.
 *
 * Each character device is a tiny file_ops vtable; a registry binds a leaf
 * name to its ops and a {major,minor}. open("/dev/<name>") allocates a
 * descriptor and points its ops/priv at the registry entry (FD_TYPE_DEVICE),
 * after which the generic read/write/close paths dispatch through the
 * vtable, so the device code lives nowhere in the syscall layer. The
 * namespace is virtual: /dev is not stored in KRFFS; stat and getdents
 * synthesize from this table.
 */

#include "../klog.h"
#include "devfs.h"
#include "fd.h"
#include "file.h"
#include "../proc/process.h"
#include "../mm/usercopy.h"
#include "../lib/kstring.h"
#include "../lib/krandom.h"
#include "../block/mbr.h"
#include "../lib/kerrno.h"
#include "../io/poll.h"
#include "../tty/pty.h"
#include "../block/block.h"
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

static int32_t dev_rand_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    /* Fail closed: /dev/random and /dev/urandom refuse with -EIO when the
     * CSPRNG was seeded only from boot jitter, so no weak randomness is handed
     * out for a key. With RDSEED/RDRAND present the source is trusted and reads
     * succeed. */
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

/* A character device is always ready for I/O and never blocks. */
static short dev_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    (void)e;
    if (wq) {
        *wq = NULL;
    }
    return (short)(events & (POLLIN | POLLOUT));
}

static int32_t dev_ioctl(fd_entry_t *e, uint32_t request, uint32_t arg)
{
    (void)e;
    (void)request;
    (void)arg;
    return -ENOTTY; /* these are not terminals */
}

/*
 * /dev/bootpart: the GRUB disk's FAT boot partition as a bounded raw window,
 * published by the disk mount when the MBR qualifies (devfs_bootpart_publish)
 * and absent otherwise. I/O is whole 512-byte sectors at sector-aligned
 * offsets, staged through a kernel bounce buffer because the AHCI driver
 * programs the buffer's physical address into its PRDT and must never see a
 * user pointer. Writes are write-through: both disk drivers flush the drive
 * cache after every write, so a returned write is on the platter.
 */
struct raw_window {
    int present;
    block_partition_t part;
};

/*
 * The staging buffer every raw transfer passes through, and the number of
 * sectors one driver call has.
 *
 * A user pointer can never go to a driver: AHCI programs the buffer's physical
 * address into its PRDT, so the bytes must be somewhere the kernel knows the
 * physical address of. This array is in `.bss`, which the linker places below
 * the 32 MiB identity-mapped limit, so its virtual address is its physical one
 * and it is contiguous.
 *
 * Sixty-four sectors is one command for both drivers: ATA chunks at 256 and
 * AHCI refuses above 8192. The size is what makes a whole-disk reflash
 * finishable at all. One sector per call turned a 500 MiB write into about a
 * million driver calls, and both drivers flush the drive's cache after every
 * write command, so it was a million cache flushes too; at 64 sectors it is
 * sixteen thousand.
 *
 * One buffer is safe for all of them because a syscall cannot be preempted:
 * the entry path sets `in_syscall` and the timer refuses to schedule a process
 * that is in one, so a second writer cannot be running while this is in use.
 * Interrupts do still run, so no interrupt path may touch it.
 */
#define RAW_BATCH_SECTORS DEVFS_RAW_BATCH_SECTORS
static uint8_t raw_batch[RAW_BATCH_SECTORS * BLOCK_SECTOR_SIZE] __attribute__((aligned(4096)));

/* How many driver calls the raw windows have made. Batching is otherwise
 * invisible from outside: a transfer of any size returns the same answer
 * whether it took one command or two hundred, and only the time differs. This
 * is reported by /proc/disk so a test can require the batching, not the
 * result, which is the difference between a 500 MiB reflash taking sixteen
 * thousand driver calls and a million. */
static uint64_t raw_calls;

uint64_t devfs_raw_calls(void)
{
    return raw_calls;
}

static struct raw_window bootpart;

/*
 * /dev/grubcore: the post-MBR gap, LBA 1 through 2047, as a bounded raw
 * window. GRUB's core image lives there, outside every partition, so updating
 * the boot loader needs a device and /dev/bootpart cannot be it: that one is
 * relative to partition 1 and the gap is before it. The range is a constant of
 * the layout instead of anything the partition table says, so a damaged table
 * cannot widen it. Sector 0 is deliberately outside: it has the partition
 * table, and a disk writes whole sectors.
 */
static struct raw_window grubcore;


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
static int32_t raw_window_read(struct raw_window *w, fd_entry_t *e, char __user *buf,
                               uint32_t count)
{
    block_device_t *part = &w->part.device;
    uint64_t off = e->ofd->raw_offset;
    uint64_t total = part->sector_count;
    uint64_t lba;
    uint32_t want, done;

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
    while (done < want) {
        uint32_t chunk = want - done;

        if (chunk > RAW_BATCH_SECTORS) {
            chunk = RAW_BATCH_SECTORS;
        }
        raw_calls++;
        if (block_request(part, 0, lba + done, chunk, raw_batch) != 0) {
            break;
        }
        if (copy_to_user((void __user *)(buf + done * BLOCK_SECTOR_SIZE), raw_batch,
                         chunk * BLOCK_SECTOR_SIZE) != 0) {
            if (done == 0) {
                return -EFAULT;
            }
            break;
        }
        done += chunk;
    }
    if (done == 0) {
        return -EIO;
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
 * gone. That batch is `RAW_BATCH_SECTORS` sectors, or fewer if it was the last
 * one the request had room for. Everything past it was never attempted. A
 * caller continues by writing again from the returned offset, which covers the
 * unknown batch as well as the rest. Batching widened that unknown range from
 * the single sector it used to be; narrowing it back would take a block
 * interface that can say how much of a command completed, which this one
 * cannot.
 *
 * The effective uid is checked here, not trusted from the open. The
 * mode stops a non-root caller at open time and nothing else: the generic
 * credential check returns success the moment the uid is zero, so a process
 * can open this device while privileged, drop privilege, and keep the
 * descriptor. Checking at write time is what makes that useless.
 */
static int32_t raw_window_write(struct raw_window *w, fd_entry_t *e, const char __user *buf,
                                uint32_t count)
{
    block_device_t *part = &w->part.device;
    uint64_t off = e->ofd->raw_offset;
    uint64_t total = part->sector_count;
    uint64_t lba;
    uint32_t want, done;

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
    while (done < want) {
        uint32_t chunk = want - done;

        if (chunk > RAW_BATCH_SECTORS) {
            chunk = RAW_BATCH_SECTORS;
        }
        if (copy_from_user(raw_batch, (const void __user *)(buf + done * BLOCK_SECTOR_SIZE),
                           chunk * BLOCK_SECTOR_SIZE) != 0) {
            if (done == 0) {
                return -EFAULT;
            }
            break;
        }
        raw_calls++;
        if (block_request(part, 1, lba + done, chunk, raw_batch) != 0) {
            break;
        }
        done += chunk;
    }
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

/* The four MBR partitions as read-only raw windows, `/dev/disk0p1` to `p4`.
 * A write answers EPERM in every build: the writable windows exist under
 * their own names, and a whole-disk write belongs to recovery. */
static struct raw_window part_win[4];

static int32_t dev_part_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -EPERM;
}

static int32_t dev_part1_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return raw_window_read(&part_win[0], e, buf, count);
}

static int32_t dev_part2_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return raw_window_read(&part_win[1], e, buf, count);
}

static int32_t dev_part3_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return raw_window_read(&part_win[2], e, buf, count);
}

static int32_t dev_part4_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return raw_window_read(&part_win[3], e, buf, count);
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
 * second writer is refused, not serialized. Readers are not counted: a
 * read alongside a write returns a torn region, which is the reader's business,
 * and hashing a region after a write is what a repair does.
 */
static int raw_writer_held;

static void raw_close(fd_entry_t *e)
{
    if (e->flags & FD_FLAG_WRITE) {
        raw_writer_held = 0;
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

static void dev_disk0_close(fd_entry_t *e)
{
    raw_close(e);
}

static void dev_bootpart_close(fd_entry_t *e)
{
    raw_close(e);
}

static const struct file_ops null_ops = {dev_eof_read, dev_sink_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops zero_ops = {dev_zero_read, dev_sink_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops full_ops = {dev_zero_read, dev_full_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops rand_ops = {dev_rand_read, dev_sink_write, dev_poll, dev_ioctl, NULL};
/*
 * The whole disk as a raw window. On an everyday kernel it exists only on a
 * boot that asked for it (`rach2.diskdiag`), so no image grows one by accident;
 * on the recovery build it exists on every boot, because rewriting this disk is
 * the only reason that build exists and the day it is needed is no time to
 * discover that a menu entry was missing a token.
 *
 * Reads serve the milestone 24 diagnostic, which answers what this machine's
 * drive is and what it has without mounting it, since mounting stamps a dirty
 * flag and that is a write. Writes belong to the recovery build alone: an
 * ordinary kernel compiles a handler that returns `-EPERM`, and the rescue
 * shell's own destructive command is compiled out of it too, so the refusal is
 * a property of the kernel, not of one function. `sys/block/recovery.h`
 * says why that is decided when the kernel is built and not when it boots.
 *
 * The mode is the weakest of the barriers and is not one at all for root, whose
 * credential check passes whatever the mode says. That is why the recovery
 * write handler asks the effective uid again at write time instead of trusting
 * the open.
 */
static struct {
    int present;
    block_device_t *whole;
    uint64_t window_sectors;
} disk0;

static int32_t dev_disk0_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    block_device_t *dev = disk0.whole;
    uint64_t off = e->ofd->raw_offset;
    uint64_t total = disk0.window_sectors;
    uint64_t lba;
    uint32_t want, done;

    if (!disk0.present || !dev) {
        return -ENXIO;
    }
    if ((off % BLOCK_SECTOR_SIZE) != 0 || (count % BLOCK_SECTOR_SIZE) != 0) {
        return -EINVAL;
    }
    lba = off / BLOCK_SECTOR_SIZE;
    if (lba >= total) {
        return 0; /* the end of the window, the regular-file convention */
    }
    want = count / BLOCK_SECTOR_SIZE;
    if ((uint64_t)want > total - lba) {
        want = (uint32_t)(total - lba);
    }
    done = 0;
    while (done < want) {
        uint32_t chunk = want - done;

        if (chunk > RAW_BATCH_SECTORS) {
            chunk = RAW_BATCH_SECTORS;
        }
        raw_calls++;
        if (dev->read(dev, lba + done, chunk, raw_batch) != 0) {
            break;
        }
        if (copy_to_user((void __user *)(buf + done * BLOCK_SECTOR_SIZE), raw_batch,
                         chunk * BLOCK_SECTOR_SIZE) != 0) {
            if (done == 0) {
                return -EFAULT;
            }
            break;
        }
        done += chunk;
    }
    if (done == 0) {
        return -EIO;
    }
    e->ofd->raw_offset = off + (uint64_t)done * BLOCK_SECTOR_SIZE;
    return (int32_t)(done * BLOCK_SECTOR_SIZE);
}

/**
 * Write whole sectors anywhere on the disk, in a recovery build only.
 *
 * This is the one place in the kernel that can destroy the machine outright,
 * so which kernel it is compiled into is decided when the kernel is built and
 * not when it boots. An everyday build compiles the refusal below and has no
 * path around it; a `-DRACH2_RECOVERY` build compiles the body. Both carry a
 * banner naming which they are, and `check_recovery.sh` reads it off the
 * artifact in both directions.
 */
#ifdef RACH2_RECOVERY
static int32_t dev_disk0_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    block_device_t *dev = disk0.whole;
    uint64_t off = e->ofd->raw_offset;
    uint64_t total = disk0.window_sectors;
    uint64_t lba;
    uint32_t want, done;

    if (!disk0.present || !dev) {
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
    while (done < want) {
        uint32_t chunk = want - done;

        if (chunk > RAW_BATCH_SECTORS) {
            chunk = RAW_BATCH_SECTORS;
        }
        if (copy_from_user(raw_batch, (const void __user *)(buf + done * BLOCK_SECTOR_SIZE),
                           chunk * BLOCK_SECTOR_SIZE) != 0) {
            if (done == 0) {
                return -EFAULT;
            }
            break;
        }
        raw_calls++;
        if (dev->write(dev, lba + done, chunk, raw_batch) != 0) {
            break;
        }
        done += chunk;
    }
    if (done == 0) {
        return -EIO;
    }
    e->ofd->raw_offset = off + (uint64_t)done * BLOCK_SECTOR_SIZE;
    return (int32_t)(done * BLOCK_SECTOR_SIZE);
}
#else
static int32_t dev_disk0_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -EPERM;
}
#endif

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
 * machine has said since, and not the debug shell's own output. The name is
 * older than that distinction.
 */
/**
 * Copy from the log's ring into a reader's buffer.
 *
 * The descriptor's offset counts from the oldest record the ring still holds,
 * which is what `klog_read` takes. A machine printing hard while a reader
 * walks the log can evict records between two reads, and the reader then sees
 * a later part of the log than it asked for; there is no cursor that survives
 * that, and a reader that cannot accept it asks for the whole ring at once.
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
        n = klog_read(chunk, want, e->ofd->offset + done);
        if (n == 0) {
            break; /* the end of the log, the regular-file convention */
        }
        if (copy_to_user((void __user *)(buf + done), chunk, n) != 0) {
            /* The bytes already copied are the caller's, so they are
               reported and the cursor moves past them; a fault after some of
               a read is a short read, not a repeat of what was delivered. */
            e->ofd->offset += done;
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        done += n;
    }
    e->ofd->offset += done;
    return (int32_t)done;
}

static const struct file_ops grubcore_ops = {dev_grubcore_read, dev_grubcore_write, dev_poll,
                                             dev_ioctl, dev_grubcore_close};
static const struct file_ops bootpart_ops = {dev_bootpart_read, dev_bootpart_write, dev_poll,
                                             dev_ioctl, dev_bootpart_close};
static const struct file_ops part1_ops = {dev_part1_read, dev_part_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops part2_ops = {dev_part2_read, dev_part_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops part3_ops = {dev_part3_read, dev_part_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops part4_ops = {dev_part4_read, dev_part_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops bootlog_ops = {dev_bootlog_read, dev_sink_write, dev_poll, dev_ioctl,
                                            NULL};
static const struct file_ops disk0_ops = {dev_disk0_read, dev_disk0_write, dev_poll, dev_ioctl,
                                          dev_disk0_close};

/*
 * The registry. major/minor are the real Linux mem-device numbers (major 1) so
 * stat reports a faithful rdev. A device has no per-fd heap state, so close is
 * NULL (nothing to release) and priv points at the registry entry itself, which
 * fstat reads for the rdev. `mode` is the nine-bit permission stat reports and
 * open enforces (the entries' owner is root:root); everything the machine
 * always had is 0666, the Linux modes for these devices, and only the two raw
 * disk windows are root-only. `bootpart` and `disk0` are both dynamically
 * present, which is why enumeration goes through `devfs_dirent` and skips
 * absent entries instead of stopping at the first one: with two of them, a
 * machine that has the second and not the first (the metal profile, which
 * never mounts) would otherwise have a device it could open and could not
 * list.
 */
struct devfs_entry {
    const char *name;
    uint16_t major;
    uint16_t minor;
    uint8_t lseek_zero; /* mem devices seek-to-zero; the rest are not seekable */
    uint16_t mode;
    const struct file_ops *ops;
};

/* ops == NULL marks a console-backed entry: opening it yields an
 * FD_TYPE_CONSOLE descriptor (the existing terminal I/O), not an ops-driven
 * device. bootpart's major is 240, the local/experimental block, minor 0. */
static const struct devfs_entry dev_table[] = {{"null", 1, 3, 1, 0666, &null_ops},
                                               {"zero", 1, 5, 1, 0666, &zero_ops},
                                               {"full", 1, 7, 1, 0666, &full_ops},
                                               {"random", 1, 8, 0, 0666, &rand_ops},
                                               {"urandom", 1, 9, 0, 0666, &rand_ops},
                                               {"console", 5, 1, 0, 0666, NULL},
                                               {"tty", 5, 0, 0, 0666, NULL},
                                               {"ptmx", 5, 2, 0, 0666, NULL},
                                               {"bootpart", 240, 0, 0, 0600, &bootpart_ops},
                                               {"disk0", 240, 1, 0, 0400, &disk0_ops},
                                               {"bootlog", 240, 2, 0, 0400, &bootlog_ops},
                                               {"grubcore", 240, 3, 0, 0600, &grubcore_ops},
                                               {"disk0p1", 240, 4, 0, 0400, &part1_ops},
                                               {"disk0p2", 240, 5, 0, 0400, &part2_ops},
                                               {"disk0p3", 240, 6, 0, 0400, &part3_ops},
                                               {"disk0p4", 240, 7, 0, 0400, &part4_ops}};

#define DEVFS_NDEV ((int)(sizeof(dev_table) / sizeof(dev_table[0])))

/** Whether table entry `i` exists right now (bootpart is mount-dependent). */
static int devfs_entry_present(int i)
{
    if (dev_table[i].ops == &bootpart_ops) {
        return bootpart.present;
    }
    if (dev_table[i].ops == &grubcore_ops) {
        return grubcore.present;
    }
    if (dev_table[i].ops == &disk0_ops) {
        return disk0.present;
    }
    if (dev_table[i].ops == &part1_ops) {
        return part_win[0].present;
    }
    if (dev_table[i].ops == &part2_ops) {
        return part_win[1].present;
    }
    if (dev_table[i].ops == &part3_ops) {
        return part_win[2].present;
    }
    if (dev_table[i].ops == &part4_ops) {
        return part_win[3].present;
    }
    return 1;
}

/* Stable inode namespace for /dev: the directory itself, then one per
 * device. */
#define DEVFS_INO_DIR  2u
#define DEVFS_INO_BASE 16u
#define DEVFS_INO(idx) (DEVFS_INO_BASE + (uint32_t)(idx))

/* Equal NUL-terminated strings? (The kernel has no strcmp; do_open compares
 * device names by hand too.) */
static int name_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static const struct devfs_entry *devfs_find(const char *name)
{
    int i;
    for (i = 0; i < DEVFS_NDEV; i++) {
        if (name_eq(dev_table[i].name, name)) {
            return devfs_entry_present(i) ? &dev_table[i] : NULL;
        }
    }
    return NULL;
}

/**
 * Open a /dev leaf by name and return its descriptor.
 *
 * `name` is the leaf under /dev, `flags` the Linux O_* set. Most entries bind
 * an fd to the registry vtable (FD_TYPE_DEVICE); three are special. /dev/ptmx
 * is a clone device that mints a fresh pty pair per open; /dev/console and
 * /dev/tty map to the kernel console (FD_TYPE_CONSOLE), except that /dev/tty
 * resolves to the caller's controlling terminal, which may be a pty rather than
 * the console.
 *
 * @return the new descriptor, or a negative errno.
 */
int devfs_open(const char *name, int flags)
{
    const struct devfs_entry *dev;
    fd_entry_t *e;
    int fd;

    /* /dev/ptmx is a clone device: each open mints a NEW pty pair and returns
     * its master fd (pty_open_master does its own fd allocation and ops
     * binding). */
    if (name_eq(name, "ptmx")) {
        if (flags & DEVFS_O_DIRECTORY) {
            return -ENOTDIR;
        }
        return pty_open_master(flags);
    }

    dev = devfs_find(name);
    if (!dev) {
        return -ENOENT;
    }
    /* The device exists, so honour the flags that contradict that. An exclusive
     * create of an existing object is -EEXIST regardless of the other flags, so
     * that comes first; then it is not a directory. */
    if ((flags & DEVFS_O_CREAT) && (flags & DEVFS_O_EXCL)) {
        return -EEXIST;
    }
    if (flags & DEVFS_O_DIRECTORY) {
        return -ENOTDIR;
    }

    /* /dev/tty is the CALLER's controlling terminal (POSIX), not
     * unconditionally the console. A job-control shell over a pty (dropbear's
     * busybox ash) opens /dev/tty to read/set the foreground process group; it
     * must get its pty, or its setjobctl loop never sees itself as foreground
     * and stops on SIGTTIN. The console owns the session
     * identified by console_tty.sid; otherwise a pty slave owns the
     * caller's session;
     * otherwise the caller has no ctty (-ENXIO, which is also what makes
     * dropbear's post-setsid /dev/tty probe come back empty as it expects).
     * /dev/console (dev->ops==NULL too) is unaffected. */
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
    if ((flags & 3) != 0 && raw_writer_held &&
        (dev->ops == &disk0_ops || dev->ops == &bootpart_ops || dev->ops == &grubcore_ops)) {
        return -EBUSY;
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
        e->priv = (void *)dev;
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
    if ((e->flags & FD_FLAG_WRITE) &&
        (dev->ops == &disk0_ops || dev->ops == &bootpart_ops || dev->ops == &grubcore_ops)) {
        raw_writer_held = 1;
    }

    return fd;
}

/**
 * Synthesize stat fields for /dev or a device leaf.
 *
 * An empty `name` is /dev itself, a directory; otherwise it names a registry
 * leaf, a character device. Each out pointer is optional. rdev packs the
 * entry's {major,minor} the way the Linux mem devices number themselves.
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
        *mode = 0020000u | (uint32_t)dev->mode; /* S_IFCHR | the entry's mode */
    }
    if (rdev) {
        *rdev = ((uint32_t)dev->major << 8) | (uint32_t)dev->minor;
    }
    if (ino) {
        *ino = DEVFS_INO((int)(dev - dev_table));
    }
    return 0;
}

uint32_t devfs_rdev(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->priv) {
        return 0;
    }
    dev = (const struct devfs_entry *)e->priv;
    return ((uint32_t)dev->major << 8) | (uint32_t)dev->minor;
}

uint32_t devfs_fd_ino(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->priv) {
        return 0;
    }
    dev = (const struct devfs_entry *)e->priv;
    return DEVFS_INO((int)(dev - dev_table));
}

int devfs_lseek_zero(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->priv) {
        return 0;
    }
    dev = (const struct devfs_entry *)e->priv;
    return dev->lseek_zero ? 1 : 0;
}

const char *devfs_fd_name(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->priv) {
        return "?";
    }
    dev = (const struct devfs_entry *)e->priv;
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
            /* The TABLE index, not the listing position: a device's inode must
             * be the same number `stat` reports, or the identity comparison
             * that `ttyname` makes between a listing and a stat would fail as
             * soon as an absent entry shifted the listing. */
            *ino = DEVFS_INO(i);
            return 1;
        }
        n--;
    }
    return 0;
}

const char *devfs_name(int i)
{
    if (i < 0 || i >= DEVFS_NDEV || !devfs_entry_present(i)) {
        return NULL;
    }
    return dev_table[i].name;
}

uint32_t devfs_fd_mode(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->priv) {
        return 0;
    }
    dev = (const struct devfs_entry *)e->priv;
    return 0020000u | (uint32_t)dev->mode;
}

/**
 * Withdraw /dev/disk0.
 *
 * The window has a pointer to the block device it reads through, and a
 * driver that is re-initialised clears the state behind that pointer. Anything
 * that resets a controller takes this node down with the other two raw
 * windows. A no-op when the device was never published.
 */
void devfs_disk_withdraw(void)
{
    disk0.present = 0;
    disk0.whole = 0;
}

void devfs_disk_publish(struct block_device *whole, uint64_t window_sectors)
{
    if (!whole || window_sectors == 0) {
        klog(KLOG_DEBUG, "devfs", "no whole-disk window to publish");
        return;
    }
    disk0.whole = whole;
    disk0.window_sectors = window_sectors;
    disk0.present = 1;
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

    if (!e || e->type != FD_TYPE_DEVICE || !e->priv) {
        return 0;
    }
    dev = (const struct devfs_entry *)e->priv;
    if (dev->ops == &bootlog_ops) {
        if (size) {
            *size = klog_len();
        }
        return 1;
    }
    return 0;
}

/**
 * Publish `/dev/disk0pN` for every non-empty MBR entry, over `backing`.
 *
 * The nodes are read-only; `mount(2)` takes its source from them. With
 * `rach2.blocklag` the backing is the lag device, so a read through a node
 * sleeps, which is what the lag exists to prove.
 */
void devfs_parts_publish(struct block_device *backing, const struct mbr_part *entries)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (entries[i].sector_count == 0) {
            part_win[i].present = 0;
            continue;
        }
        block_partition_init(&part_win[i].part, backing, entries[i].start_lba,
                             entries[i].sector_count);
        part_win[i].present = 1;
    }
}

void devfs_bootpart_publish(struct block_device *backing, uint32_t start_lba, uint32_t sector_count)
{
    /* The boot partition is 16 MiB on a test image and 64 MiB on the sealed
     * disk. The bound is kept at 2 GiB so the legacy 32-bit lseek can still
     * name every byte of this particular window; the wider windows are what
     * _llseek exists for. Anything larger than the bound is a misbuilt disk. */
    if (sector_count == 0 || sector_count >= (0x80000000u / BLOCK_SECTOR_SIZE)) {
        klog(KLOG_WARN, "devfs",
             "boot partition too large; /dev/bootpart absent");
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
        klog(KLOG_WARN, "devfs",
             "entry 0 has the root; /dev/bootpart withdrawn");
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
        klog(KLOG_WARN, "devfs",
             "no room for the boot loader gap; /dev/grubcore absent");
        return;
    }
    block_partition_init(&grubcore.part, backing, MBR_GAP_FIRST_LBA,
                         MBR_GAP_END_LBA - MBR_GAP_FIRST_LBA);
    grubcore.present = 1;
}

int devfs_fd_is_raw(struct fd_entry *e)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->priv) {
        return 0;
    }
    dev = (const struct devfs_entry *)e->priv;
    return dev->ops == &bootpart_ops || dev->ops == &grubcore_ops ||
           dev->ops == &disk0_ops || dev->ops == &part1_ops ||
           dev->ops == &part2_ops || dev->ops == &part3_ops ||
           dev->ops == &part4_ops;
}

int devfs_fd_raw_size(struct fd_entry *e, uint64_t *size)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->priv) {
        return 0;
    }
    dev = (const struct devfs_entry *)e->priv;
    if (dev->ops == &part1_ops || dev->ops == &part2_ops ||
        dev->ops == &part3_ops || dev->ops == &part4_ops) {
        int idx = dev->ops == &part1_ops   ? 0
                  : dev->ops == &part2_ops ? 1
                  : dev->ops == &part3_ops ? 2
                                           : 3;
        if (!part_win[idx].present) {
            return 0;
        }
        if (size) {
            *size = part_win[idx].part.device.sector_count * BLOCK_SECTOR_SIZE;
        }
        return 1;
    }
    if (dev->ops == &bootpart_ops && bootpart.present) {
        if (size) {
            *size = bootpart.part.device.sector_count * BLOCK_SECTOR_SIZE;
        }
        return 1;
    }
    if (dev->ops == &grubcore_ops && grubcore.present) {
        if (size) {
            *size = grubcore.part.device.sector_count * BLOCK_SECTOR_SIZE;
        }
        return 1;
    }
    if (dev->ops == &disk0_ops && disk0.present) {
        if (size) {
            *size = disk0.window_sectors * BLOCK_SECTOR_SIZE;
        }
        return 1;
    }
    return 0;
}
