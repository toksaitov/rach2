/*
 * devfs.c: a synthetic /dev built on the file_ops dispatch.
 *
 * Each character device is a tiny file_ops vtable; a registry binds a leaf name to
 * its ops and a {major,minor}. open("/dev/<name>") allocates a descriptor and
 * points its ops/priv at the registry entry (FD_TYPE_DEVICE), after which the
 * generic read/write/close paths dispatch through the vtable, so the device code
 * lives nowhere in the syscall layer. The namespace is virtual: /dev is not stored
 * in KRFFS; stat and getdents synthesize from this table.
 */

#include "devfs.h"
#include "fd.h"
#include "file.h"
#include "../proc/process.h"
#include "../mm/usercopy.h"
#include "../lib/kstring.h"
#include "../lib/krandom.h"
#include "../lib/kerrno.h"
#include "../io/poll.h"
#include "../tty/pty.h"
#include "../block/block.h"
#include "../serial.h"

/* Linux O_* bits devfs_open honours (the rest are irrelevant to a char device). */
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
    /* Fail closed: /dev/random and /dev/urandom refuse with -EIO when the CSPRNG
     * was seeded only from boot jitter, so no weak randomness is handed out for a
     * key. With RDSEED/RDRAND present the source is trusted and reads succeed. */
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
static struct {
    int present;
    block_partition_t part;
} bootpart;

static int32_t dev_bootpart_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    uint8_t sector[BLOCK_SECTOR_SIZE];
    block_device_t *part = &bootpart.part.device;
    uint32_t off = e->ofd->offset;
    uint32_t total = (uint32_t)part->sector_count;
    uint32_t lba, want, done;

    if (!bootpart.present) {
        return -ENXIO;
    }
    if ((off % BLOCK_SECTOR_SIZE) != 0 || (count % BLOCK_SECTOR_SIZE) != 0) {
        return -EINVAL;
    }
    lba = off / BLOCK_SECTOR_SIZE;
    if (lba >= total) {
        return 0; /* end of the partition, the regular-file convention */
    }
    want = count / BLOCK_SECTOR_SIZE;
    if (want > total - lba) {
        want = total - lba;
    }
    for (done = 0; done < want; done++) {
        if (part->read(part, lba + done, 1, sector) != 0) {
            break;
        }
        if (copy_to_user((void __user *)(buf + done * BLOCK_SECTOR_SIZE), sector,
                         BLOCK_SECTOR_SIZE) != 0) {
            if (done == 0) {
                return -EFAULT;
            }
            break;
        }
    }
    if (done == 0) {
        return -EIO;
    }
    e->ofd->offset = off + done * BLOCK_SECTOR_SIZE;
    return (int32_t)(done * BLOCK_SECTOR_SIZE);
}

static int32_t dev_bootpart_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    uint8_t sector[BLOCK_SECTOR_SIZE];
    block_device_t *part = &bootpart.part.device;
    uint32_t off = e->ofd->offset;
    uint32_t total = (uint32_t)part->sector_count;
    uint32_t lba, want, done;

    if (!bootpart.present) {
        return -ENXIO;
    }
    if ((off % BLOCK_SECTOR_SIZE) != 0 || (count % BLOCK_SECTOR_SIZE) != 0) {
        return -EINVAL;
    }
    if (count == 0) {
        return 0;
    }
    lba = off / BLOCK_SECTOR_SIZE;
    want = count / BLOCK_SECTOR_SIZE;
    /* A write that starts at or crosses the end writes nothing: the boot
       partition's size is fixed and a partial install write helps nobody. */
    if (lba >= total || want > total - lba) {
        return -ENOSPC;
    }
    for (done = 0; done < want; done++) {
        if (copy_from_user(sector, (const void __user *)(buf + done * BLOCK_SECTOR_SIZE),
                           BLOCK_SECTOR_SIZE) != 0) {
            if (done == 0) {
                return -EFAULT;
            }
            break;
        }
        if (part->write(part, lba + done, 1, sector) != 0) {
            break;
        }
    }
    if (done == 0) {
        return -EIO;
    }
    e->ofd->offset = off + done * BLOCK_SECTOR_SIZE;
    return (int32_t)(done * BLOCK_SECTOR_SIZE);
}

static const struct file_ops null_ops = {dev_eof_read, dev_sink_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops zero_ops = {dev_zero_read, dev_sink_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops full_ops = {dev_zero_read, dev_full_write, dev_poll, dev_ioctl, NULL};
static const struct file_ops rand_ops = {dev_rand_read, dev_sink_write, dev_poll, dev_ioctl, NULL};
/*
 * The whole disk, read-only, for the milestone 24 diagnostic. It exists only
 * on a boot that asked for it (`rach2.diskdiag`), so no image grows a raw
 * whole-disk node by accident.
 *
 * The write is refused here as well as everywhere else. Three refusals already
 * stand between a caller and a written sector: the 0400 mode fails an
 * `open(O_WRONLY)`, the descriptor carries no write flag, and under the metal
 * build the driver's allowlist refuses the command. This is the fourth, and it
 * is the one that does not depend on which kernel is running.
 */
static struct {
    int present;
    block_device_t *whole;
    uint32_t window_sectors;
} disk0;

static int32_t dev_disk0_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    uint8_t sector[BLOCK_SECTOR_SIZE];
    block_device_t *dev = disk0.whole;
    uint32_t off = e->ofd->offset;
    uint32_t total = disk0.window_sectors;
    uint32_t lba, want, done;

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
    if (want > total - lba) {
        want = total - lba;
    }
    for (done = 0; done < want; done++) {
        if (dev->read(dev, (uint64_t)(lba + done), 1, sector) != 0) {
            break;
        }
        if (copy_to_user((void __user *)(buf + done * BLOCK_SECTOR_SIZE), sector,
                         BLOCK_SECTOR_SIZE) != 0) {
            if (done == 0) {
                return -EFAULT;
            }
            break;
        }
    }
    if (done == 0) {
        return -EIO;
    }
    e->ofd->offset = off + done * BLOCK_SECTOR_SIZE;
    return (int32_t)(done * BLOCK_SECTOR_SIZE);
}

static int32_t dev_disk0_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -EPERM;
}

static const struct file_ops bootpart_ops = {dev_bootpart_read, dev_bootpart_write, dev_poll,
                                             dev_ioctl, NULL};
static const struct file_ops disk0_ops = {dev_disk0_read, dev_disk0_write, dev_poll, dev_ioctl,
                                          NULL};

/*
 * The registry. major/minor are the real Linux mem-device numbers (major 1) so
 * stat reports a faithful rdev. A device has no per-fd heap state, so close is
 * NULL (nothing to release) and priv points at the registry entry itself, which
 * fstat reads for the rdev. `mode` is the nine-bit permission stat reports and
 * open enforces (the entries' owner is root:root); everything the machine
 * always had is 0666, the Linux modes for these devices, and only the two raw
 * disk windows are root-only. `bootpart` and `disk0` are both dynamically
 * present, which is why enumeration goes through `devfs_dirent` and skips
 * absent entries rather than stopping at the first one: with two of them, a
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

/* ops == NULL marks a console-backed entry: opening it yields an FD_TYPE_CONSOLE
 * descriptor (the existing terminal I/O), not an ops-driven device. bootpart's
 * major is 240, the local/experimental block, minor 0. */
static const struct devfs_entry dev_table[] = {{"null", 1, 3, 1, 0666, &null_ops},
                                               {"zero", 1, 5, 1, 0666, &zero_ops},
                                               {"full", 1, 7, 1, 0666, &full_ops},
                                               {"random", 1, 8, 0, 0666, &rand_ops},
                                               {"urandom", 1, 9, 0, 0666, &rand_ops},
                                               {"console", 5, 1, 0, 0666, NULL},
                                               {"tty", 5, 0, 0, 0666, NULL},
                                               {"ptmx", 5, 2, 0, 0666, NULL},
                                               {"bootpart", 240, 0, 0, 0600, &bootpart_ops},
                                               {"disk0", 240, 1, 0, 0400, &disk0_ops}};

#define DEVFS_NDEV ((int)(sizeof(dev_table) / sizeof(dev_table[0])))

/** Whether table entry `i` exists right now (bootpart is mount-dependent). */
static int devfs_entry_present(int i)
{
    if (dev_table[i].ops == &bootpart_ops) {
        return bootpart.present;
    }
    if (dev_table[i].ops == &disk0_ops) {
        return disk0.present;
    }
    return 1;
}

/* Stable inode namespace for /dev: the directory itself, then one per device. */
#define DEVFS_INO_DIR  2u
#define DEVFS_INO_BASE 16u
#define DEVFS_INO(idx) (DEVFS_INO_BASE + (uint32_t)(idx))

/* Equal NUL-terminated strings? (The kernel has no strcmp; do_open compares device
 * names by hand too.) */
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
 * `name` is the leaf under /dev, `flags` the Linux O_* set. Most entries bind an
 * fd to the registry vtable (FD_TYPE_DEVICE); three are special. /dev/ptmx is a
 * clone device that mints a fresh pty pair per open; /dev/console and /dev/tty
 * map to the kernel console (FD_TYPE_CONSOLE), except that /dev/tty resolves to
 * the caller's controlling terminal, which may be a pty rather than the console.
 *
 * @return the new descriptor, or a negative errno.
 */
int devfs_open(const char *name, int flags)
{
    const struct devfs_entry *dev;
    fd_entry_t *e;
    int fd;

    /* /dev/ptmx is a clone device: each open mints a NEW pty pair and returns its
     * master fd (pty_open_master does its own fd allocation and ops binding). */
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

    /* /dev/tty is the CALLER's controlling terminal (POSIX), not unconditionally
     * the console. A job-control shell over a pty (dropbear's busybox ash) opens
     * /dev/tty to read/set the foreground process group; it must get its pty, or
     * its setjobctl loop never sees itself as foreground and stops on SIGTTIN. The
     * console owns the session whose leader holds console_tty.sid; otherwise a pty
     * slave owns the caller's session; otherwise the caller has no ctty (-ENXIO,
     * which is also what makes dropbear's post-setsid /dev/tty probe come back
     * empty as it expects). /dev/console (dev->ops==NULL too) is unaffected. */
    if (dev->ops == NULL && name_eq(name, "tty")) {
        int sid = current_proc ? (int)current_proc->sid : 0;
        if (console_tty.sid == 0 || console_tty.sid != sid) {
            int idx = pty_session_slave(sid);
            if (idx >= 0) {
                return pty_open_slave(idx, flags);
            }
            return -ENXIO; /* the caller has no controlling terminal */
        }
        /* ctty is the console -> fall through to the FD_TYPE_CONSOLE path below. */
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
        /* /dev/console and /dev/tty reuse the kernel console (enum dispatch). */
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

    return fd;
}

/**
 * Synthesize stat fields for /dev or a device leaf.
 *
 * An empty `name` is /dev itself, a directory; otherwise it names a registry
 * leaf, a character device. Each out pointer is optional. rdev packs the entry's
 * {major,minor} the way the Linux mem devices number themselves.
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

void devfs_disk_publish(struct block_device *whole, uint32_t window_sectors)
{
    if (!whole || window_sectors == 0) {
        serial_printf("devfs: no whole-disk window to publish\n");
        return;
    }
    disk0.whole = whole;
    disk0.window_sectors = window_sectors;
    disk0.present = 1;
}

int devfs_fd_disk_size(struct fd_entry *e, uint32_t *size)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->priv) {
        return 0;
    }
    dev = (const struct devfs_entry *)e->priv;
    if (dev->ops != &disk0_ops || !disk0.present) {
        return 0;
    }
    *size = disk0.window_sectors * BLOCK_SECTOR_SIZE;
    return 1;
}

void devfs_bootpart_publish(struct block_device *backing, uint32_t start_lba, uint32_t sector_count)
{
    /* Byte offsets ride the descriptor's 32-bit offset and lseek's int32
     * result, so the partition must sit under 2 GiB; the real boot partition
     * is 16 MiB and anything larger than the bound is a misbuilt disk. */
    if (sector_count == 0 || sector_count >= (0x80000000u / BLOCK_SECTOR_SIZE)) {
        serial_printf("devfs: boot partition too large; /dev/bootpart absent\n");
        return;
    }
    block_partition_init(&bootpart.part, backing, start_lba, sector_count);
    bootpart.present = 1;
}

int devfs_fd_bootpart_size(struct fd_entry *e, uint32_t *size)
{
    const struct devfs_entry *dev;

    if (!e || e->type != FD_TYPE_DEVICE || !e->priv) {
        return 0;
    }
    dev = (const struct devfs_entry *)e->priv;
    if (dev->ops != &bootpart_ops || !bootpart.present) {
        return 0;
    }
    if (size) {
        *size = (uint32_t)bootpart.part.device.sector_count * BLOCK_SECTOR_SIZE;
    }
    return 1;
}
