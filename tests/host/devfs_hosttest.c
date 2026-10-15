/**
 * devfs_hosttest.c: the device files of rach2/sys/fs/devfs.c: the errors of
 * an exclusive create, the devices whose transfers move the description's
 * offset, and the windows /dev/grubcore and a whole-disk node publish.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/block/partition.c rach2/sys/block/gpt.c
 *
 * The test includes the source whole. A stub of a call that no row should
 * reach fails the test through `unexpected`.
 */
#include "fs/devfs.c"

extern int printf(const char *format, ...);
static int failures, master_calls, descriptor_calls;
/* Set by the rows that publish a window, which log, cut a partition view and
   copy an ioctl's answer out; every other row must reach none of those. The
   views cut are counted in `views_cut`. */
static int publishing, views_cut;
static process_t process;
struct percpu cpu_table[NR_CPUS] = {{.current = &process}};
struct tty console_tty;

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("devfs-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Count an open of a pty master. Returns descriptor 7. */
int pty_open_master(int flags)
{
    (void)flags;
    master_calls++;
    return 7;
}

/** Count a descriptor allocation, which fails. Returns -1. */
int fd_alloc(fd_table_t *table)
{
    (void)table;
    descriptor_calls++;
    return -1;
}

/** Whether a transfer of the device `name`, published or not, moves the
 * offset of its description, as `devfs_fd_moves_offset` answers. */
static int moves_offset(const char *name)
{
    fd_entry_t e;
    int i;

    k_memset(&e, 0, sizeof(e));
    e.type = FD_TYPE_DEVICE;
    for (i = 0; i < DEVFS_NDEV; i++) {
        if (name_eq(dev_table[i].name, name)) {
            e.dev = &dev_table[i];
        }
    }
    check(e.dev != NULL, "every device a row names is in the table");
    return devfs_fd_moves_offset(&e);
}

/**
 * The screen log's read sets the offset past its copy, which may sleep in a
 * fault on the caller's buffer, as the boot log's read and a boot window's
 * transfer do; the null, zero and random devices' ignore the offset.
 */
static void offset_devices(void)
{
    check(moves_offset("screenlog"),
          "a read of /dev/screenlog holds its description, as it moves the offset");
    check(moves_offset("bootlog") && moves_offset("bootpart") && moves_offset("grubcore"),
          "a transfer of the boot log or a boot window holds its description");
    check(!moves_offset("null") && !moves_offset("zero") && !moves_offset("urandom"),
          "a device whose transfers ignore the offset holds nothing");
}

static const uint8_t bios_boot_type[16] = {0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6f, 0x6e,
                                           0x74, 0x4e, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49};

/**
 * The installed disk's GUID table as the partition service reads it from the
 * primary header of a 4096-sector disk: the EFI system partition, recovery and
 * the root as entries 1 to 3, and entry 4, the BIOS boot partition, from LBA
 * 34 through 2047.
 */
static void installed_table(struct part_table *t)
{
    int i;

    k_memset(t, 0, sizeof(*t));
    t->kind = PART_GPT;
    t->gpt.header_lba = 1;
    t->gpt.alternate_lba = 4095;
    t->gpt.array_lba = 2;
    t->gpt.array_sectors = 32;
    t->gpt.first_usable = 34;
    t->gpt.last_usable = 4062;
    t->count = 4;
    for (i = 0; i < 3; i++) {
        t->parts[i].number = i + 1;
        t->parts[i].start_lba = 2048 + (uint64_t)i * 512;
        t->parts[i].sector_count = 512;
        t->parts[i].type_guid[0] = (uint8_t)(0x10 + i);
    }
    t->parts[3].number = 4;
    t->parts[3].start_lba = 34;
    t->parts[3].sector_count = 2014;
    k_memcpy(t->parts[3].type_guid, bios_boot_type, 16);
}

/** Whether the ioctls of /dev/grubcore report `first` as its first sector
 *  and `bytes` as its size, as `bootctl` reads them. */
static int window_reports(uint64_t first, uint64_t bytes)
{
    struct {
        uint8_t heads;
        uint8_t sectors;
        uint16_t cylinders;
        unsigned long start;
    } geo;
    uint64_t size = 0;
    fd_entry_t e;
    int i;

    k_memset(&e, 0, sizeof(e));
    e.type = FD_TYPE_DEVICE;
    for (i = 0; i < DEVFS_NDEV; i++) {
        if (name_eq(dev_table[i].name, "grubcore")) {
            e.dev = &dev_table[i];
        }
    }
    geo.start = 0;
    return e.dev && dev_ioctl(&e, 0x0301u, (uintptr_t)&geo) == 0 &&
           dev_ioctl(&e, 0x80081272u, (uintptr_t)&size) == 0 && geo.start == first && size == bytes;
}

/**
 * Where /dev/grubcore is. On an MBR disk it is the gap, LBA 1 through 2047,
 * on any disk that holds it; on a GPT disk it is the BIOS boot partition,
 * whether the table was read from the primary header or the backup. There is
 * none where the table has no BIOS boot partition, where the partition
 * covers the entry array or runs past the disk, and where the table is an
 * MBR's. `HDIO_GETGEO` reports the window's first sector and `BLKGETSIZE64`
 * its size, which is how `bootctl grub` lays out its tails.
 */
static void grubcore_windows(void)
{
    static struct part_table t;
    block_device_t disk;

    publishing = 1;
    k_memset(&disk, 0, sizeof(disk));

    disk.sector_count = 4096;
    devfs_grubcore_publish(&disk);
    check(grubcore.present && grubcore.part.backing == &disk && grubcore.part.start_lba == 1 &&
              grubcore.part.device.sector_count == 2047,
          "on an MBR disk /dev/grubcore is LBA 1 through 2047");
    check(window_reports(1, 2047u * 512u), "and reports its first sector, 1, and its size");
    devfs_grubcore_withdraw();
    views_cut = 0;
    disk.sector_count = 2047;
    devfs_grubcore_publish(&disk);
    check(!grubcore.present && views_cut == 0, "a disk smaller than the gap has none");

    disk.sector_count = 4096;
    installed_table(&t);
    devfs_grubcore_publish_gpt(&disk, &t);
    check(grubcore.present && grubcore.part.backing == &disk && grubcore.part.start_lba == 34 &&
              grubcore.part.device.sector_count == 2014,
          "on a GPT disk /dev/grubcore is the BIOS boot partition, LBA 34 through 2047");
    check(window_reports(34, 2014u * 512u), "and reports its first sector, 34, and its size");
    devfs_grubcore_withdraw();

    installed_table(&t);
    t.backup_used = 1;
    t.gpt.header_lba = 4095;
    t.gpt.alternate_lba = 1;
    t.gpt.array_lba = 4063;
    devfs_grubcore_publish_gpt(&disk, &t);
    check(grubcore.present && grubcore.part.start_lba == 34 &&
              grubcore.part.device.sector_count == 2014,
          "a table read from its backup gives the same window");
    devfs_grubcore_withdraw();

    views_cut = 0;
    installed_table(&t);
    t.parts[3].type_guid[0] ^= 0xff;
    devfs_grubcore_publish_gpt(&disk, &t);
    check(!grubcore.present && views_cut == 0, "a GPT disk with no BIOS boot partition has none");

    devfs_grubcore_withdraw();
    views_cut = 0;
    installed_table(&t);
    t.parts[3].start_lba = 2;
    t.parts[3].sector_count = 2046;
    t.gpt.first_usable = 2;
    devfs_grubcore_publish_gpt(&disk, &t);
    check(!grubcore.present && views_cut == 0,
          "nor one whose BIOS boot partition covers the entry array");

    devfs_grubcore_withdraw();
    views_cut = 0;
    installed_table(&t);
    disk.sector_count = 2000;
    devfs_grubcore_publish_gpt(&disk, &t);
    check(!grubcore.present && views_cut == 0, "nor one whose partition runs past the disk");

    devfs_grubcore_withdraw();
    views_cut = 0;
    installed_table(&t);
    t.kind = PART_MBR;
    disk.sector_count = 4096;
    devfs_grubcore_publish_gpt(&disk, &t);
    check(!grubcore.present && views_cut == 0, "and an MBR table never takes this path");
    publishing = 0;
}

/** No loop device is attached at `index`; no row opens one. */
block_device_t *loop_attached_at(int index)
{
    (void)index;
    return 0;
}

/** A loop node has no device: -ENXIO. */
block_device_t *loop_node_device(int index, int write, int32_t *err)
{
    (void)index;
    (void)write;
    *err = -ENXIO;
    return 0;
}

/** No file can be attached to a loop device: -ENXIO. */
int loop_attach_file(int index, const char *path, int writable, uint32_t flags)
{
    (void)index;
    (void)path;
    (void)writable;
    (void)flags;
    return -ENXIO;
}

/** An ioctl of a loop device fails with -ENXIO. */
int32_t loop_ioctl(int index, int privileged, uint32_t request, uintptr_t arg)
{
    (void)index;
    (void)privileged;
    (void)request;
    (void)arg;
    return -ENXIO;
}

/** An ioctl of /dev/loop-control fails with -ENXIO. */
int32_t loop_control_ioctl(uint32_t request, uintptr_t arg)
{
    (void)request;
    (void)arg;
    return -ENXIO;
}

/** The close of a loop node, which has nothing to do. */
void loop_node_closed(int index)
{
    (void)index;
}

/** Loop information is refused with -EINVAL. */
int loop_info_flags(const uint8_t *info, uint32_t *flags)
{
    (void)info;
    *flags = 0;
    return -EINVAL;
}

/** No descriptor link resolves: -ENOENT. */
int procfs_readlink_fd(int pid, int fd, char *buf, int cap)
{
    (void)pid;
    (void)fd;
    (void)buf;
    (void)cap;
    return -ENOENT;
}

/** No descriptor is open. */
fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    (void)fd;
    return 0;
}

/**
 * The whole-disk node of a disk past 2 TiB covers every sector, so that a
 * GUID table's backup at the disk's end can be written through it.
 */
static void whole_disk_window(void)
{
    static struct disk d;
    block_device_t disk;

    publishing = 1;
    k_memset(&disk, 0, sizeof(disk));
    k_memset(&d, 0, sizeof(d));
    disk.sector_count = 0x200000000ull;
    d.dev = &disk;
    devfs_disk_publish(&d);
    check(d.whole.present && d.whole.part.backing == &disk && d.whole.part.start_lba == 0 &&
              d.whole.part.device.sector_count == 0x200000000ull,
          "a 4 TiB disk's whole-disk node covers all of its 2^33 sectors");
    publishing = 0;
}

/**
 * The rows of an open, then those of `offset_devices`, `grubcore_windows`
 * and `whole_disk_window`. An exclusive create of ptmx, null or an absent
 * name, in each access mode and with or without O_DIRECTORY and O_NONBLOCK,
 * is EEXIST for a device and ENOENT for an absent one, and opens no pty
 * master and takes no descriptor. An open of ptmx with O_DIRECTORY is
 * ENOTDIR and opens nothing, and an ordinary open of ptmx opens a pty
 * master. Returns 0 when every check passed and 1 otherwise.
 */
int main(void)
{
    int acc, extra;

    for (acc = 0; acc < 3; acc++) {
        for (extra = 0; extra < 2; extra++) {
            int flags = acc | DEVFS_O_CREAT | DEVFS_O_EXCL |
                        (extra ? DEVFS_O_DIRECTORY | DEVFS_O_NONBLOCK : 0);
            check(devfs_open("ptmx", flags) == -EEXIST,
                  "ptmx exclusive create returns EEXIST before dispatch");
            check(devfs_open("null", flags) == -EEXIST, "existing leaf exclusive create");
            check(devfs_open("absent", flags) == -ENOENT, "absent leaf remains absent");
        }
    }
    check(!master_calls && !descriptor_calls, "exclusive create allocates nothing");
    check(devfs_open("ptmx", DEVFS_O_DIRECTORY) == -ENOTDIR && !master_calls,
          "pty directory open allocates nothing");
    check(devfs_open("ptmx", 2) == 7 && master_calls == 1, "ordinary pty open dispatches");
    offset_devices();
    grubcore_windows();
    whole_disk_window();
    if (!failures) {
        printf("devfs-hosttest: result: pass: exclusive pty creation, offset devices, "
               "boot loader windows, the whole-disk window\n");
    }
    return failures != 0;
}

/** Fail the test: a row reached a call it should not. Returns -EIO. */
static int unexpected(void)
{
    check(0, "unrelated device operation");
    return -EIO;
}

/** The transfer limit of a block device, which no row reads. */
uint32_t block_max_sectors(block_device_t *dev)
{
    (void)dev;
    return (uint32_t)unexpected();
}

/** Cut a view as the block layer does, for the rows that publish a window. */
void block_partition_init(block_partition_t *part, block_device_t *dev, uint64_t first,
                          uint64_t count)
{
    if (!publishing) {
        unexpected();
        return;
    }
    views_cut++;
    k_memset(part, 0, sizeof(*part));
    part->backing = dev;
    part->start_lba = first;
    part->device.sector_count = count;
}

/** The device under a view, which no row looks up. */
block_device_t *block_physical(block_device_t *dev)
{
    unexpected();
    return dev;
}

/** A block transfer, which no row makes. */
int block_request(block_device_t *dev, int write, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    (void)write;
    (void)lba;
    (void)count;
    (void)buf;
    return unexpected();
}

/**
 * Fail a copy no row expects. The destination is cleared first: GCC 15
 * reads a caller through this stub, and a byte the stub leaves unwritten is
 * a byte it reports as read uninitialized, as in the screen log's write.
 */
int copy_from_user(void *dst, const void __user *src, size_t size)
{
    uint32_t i;

    (void)src;
    for (i = 0; i < size; ++i) {
        ((char *)dst)[i] = 0;
    }
    return unexpected();
}

/** Copy an ioctl's answer out in a row that publishes a window, and fail
 *  as `copy_from_user` does in any other. */
int copy_to_user(void __user *dst, const void *src, size_t size)
{
    if (publishing) {
        k_memcpy((void *)dst, src, size);
        return 0;
    }
    return copy_from_user(dst, src, size);
}

/** The disk at `index`, which no row looks up. */
struct disk *disk_at(int index)
{
    (void)index;
    unexpected();
    return NULL;
}

/** The number of disks, which no row reads. */
int disk_count(void)
{
    return unexpected();
}

/** The disk of a device, which no row looks up. */
struct disk *disk_of(block_device_t *dev)
{
    (void)dev;
    unexpected();
    return NULL;
}

/** A rescan of a disk's partitions, which no row makes. */
int disk_rescan(struct disk *d)
{
    (void)d;
    return unexpected();
}

/** A page allocation, which no row makes. */
void *kalloc_pages_try(uint32_t count, uintptr_t *phys)
{
    (void)count;
    (void)phys;
    unexpected();
    return NULL;
}

/** Free pages, which no row has. */
void kfree_pages(void *ptr, uint32_t count)
{
    (void)ptr;
    (void)count;
    unexpected();
}

/** The kernel log: discarded in a row that publishes a window, and a
 *  failure in any other. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    if (!publishing) {
        unexpected();
    }
}

/** The length of the kernel log, which no row reads. */
uint32_t klog_len(void)
{
    return (uint32_t)unexpected();
}

/** A read of the kernel log, which no row makes. */
uint32_t klog_read(char *buf, uint32_t cap, uint32_t offset)
{
    (void)buf;
    (void)cap;
    (void)offset;
    return (uint32_t)unexpected();
}

/** The screen's log level, which no row reads. */
int klog_screen_level(void)
{
    return unexpected();
}

/** A change of the screen's log level, which no row makes. */
void klog_set_screen_level(int level)
{
    (void)level;
    unexpected();
}

/** A read of the real-time clock, which no row makes. */
int rtc_read_clock(rtc_time_t *out)
{
    (void)out;
    unexpected();
    return -1;
}

/** A write of the real-time clock, which no row makes. */
int rtc_write_clock(const rtc_time_t *t)
{
    (void)t;
    unexpected();
    return -1;
}

/** The weekday and the day of the year, which no row reads. */
void rtc_week_and_year_day(const rtc_time_t *t, int32_t *weekday, int32_t *yearday)
{
    (void)t;
    *weekday = 0;
    *yearday = 0;
    unexpected();
}

/** Random bytes, which no row reads. */
void krandom_bytes(void *buf, uint32_t count)
{
    (void)buf;
    (void)count;
    unexpected();
}

/** The clock time of a count of seconds, which no row converts. */
void rtc_time_of_seconds(uint32_t seconds, rtc_time_t *t)
{
    (void)seconds;
    (void)t;
    unexpected();
}

/** The seconds of a clock time, which no row converts. */
int64_t rtc_seconds_of(const rtc_time_t *t)
{
    (void)t;
    unexpected();
    return 0;
}

/** A write of the clock's alarm, which no row makes. */
void rtc_alarm_write(const rtc_time_t *t)
{
    (void)t;
    unexpected();
}

/** A read of the clock's alarm, which no row makes. */
int rtc_alarm_read(rtc_time_t *t)
{
    (void)t;
    unexpected();
    return 0;
}

/** The alarm's interrupt turned on or off, which no row does. */
void rtc_alarm_interrupts(int on)
{
    (void)on;
    unexpected();
}

/** The acknowledgement of an alarm, which no row makes. */
int rtc_alarm_ack(void)
{
    unexpected();
    return 0;
}

/** A wake from suspend, which no row makes. */
void suspend_wake(void)
{
    unexpected();
}

/** The alarm's handler attached, which no row does. */
void rtc_alarm_attach(isr_handler_t handler)
{
    (void)handler;
    unexpected();
}

/** A wait queue made, which no row needs. */
void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
    unexpected();
}

/** A sleep, which no row takes. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    unexpected();
    return 0;
}

/** Interrupts masked, which no row needs. */
uint32_t kirq_save(void)
{
    unexpected();
    return 0;
}

/** Interrupts restored, which no row needs. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
    unexpected();
}

/** A temporary map of a frame for /dev/mem, which no row reads. */
void *kmap_temp(uintptr_t phys)
{
    (void)phys;
    unexpected();
    return 0;
}

/** The end of a temporary map, which no row makes. */
void kunmap_temp(void *va)
{
    (void)va;
    unexpected();
}

/** Whether a frame is a device's, for /dev/mem, which no row reads. */
int paging_frame_is_device(uintptr_t phys)
{
    (void)phys;
    unexpected();
    return 0;
}

/** Whether the random source is hardware entropy, which no row reads. */
int krandom_is_trusted(void)
{
    return unexpected();
}

/** A write to /dev/loop-control, which no row makes. */
int loop_control_write(const char *buf, uint32_t count)
{
    (void)buf;
    (void)count;
    return unexpected();
}

/** No loop device is attached at `index`. */
block_device_t *loop_device_at(int index)
{
    (void)index;
    return NULL;
}

/** The injected failure of frame allocations, which no row arms. */
int paging_framefail_arm(uint32_t count)
{
    (void)count;
    return unexpected();
}

/** An open of a pty slave, which no row makes. */
int pty_open_slave(int index, int flags)
{
    (void)index;
    (void)flags;
    return unexpected();
}

/** The pty of a session's controlling terminal, which no row looks up. */
int pty_session_slave(int sid)
{
    (void)sid;
    return unexpected();
}

/** Whether a recovery kernel publishes the disks, which no row reads. */
int recovery_publishes_disk(void)
{
    return unexpected();
}

/** An uninterruptible sleep, which no row takes. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return unexpected();
}

/** A wake, which no row makes. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    unexpected();
}
