/**
 * Check exclusive creation before pty or descriptor allocation, and which
 * devices' transfers move the description's offset, so that read(2) and
 * write(2) hold the description for them.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "fs/devfs.c"

extern int printf(const char *format, ...);
static int failures, master_calls, descriptor_calls;
static process_t process;
struct percpu cpu_table[NR_CPUS] = {{&process}};
struct tty console_tty;

/** Record a failed invariant. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("devfs-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Count the pty allocation dispatch. */
int pty_open_master(int flags)
{
    (void)flags;
    master_calls++;
    return 7;
}

/** Count a descriptor allocation without allocating kernel storage. */
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

/** Check errno precedence, absent leaves, ordinary pty dispatch and the
 * devices whose transfers move the offset. */
/* The loop devices and the descriptor table, which no row here opens. */
block_device_t *loop_attached_at(int index)
{
    (void)index;
    return 0;
}

block_device_t *loop_node_device(int index, int write, int32_t *err)
{
    (void)index;
    (void)write;
    *err = -ENXIO;
    return 0;
}

int loop_attach_file(int index, const char *path, int writable, uint32_t flags)
{
    (void)index;
    (void)path;
    (void)writable;
    (void)flags;
    return -ENXIO;
}

int32_t loop_ioctl(int index, int privileged, uint32_t request, uintptr_t arg)
{
    (void)index;
    (void)privileged;
    (void)request;
    (void)arg;
    return -ENXIO;
}

int32_t loop_control_ioctl(uint32_t request, uintptr_t arg)
{
    (void)request;
    (void)arg;
    return -ENXIO;
}

void loop_node_closed(int index)
{
    (void)index;
}

int loop_info_flags(const uint8_t *info, uint32_t *flags)
{
    (void)info;
    *flags = 0;
    return -EINVAL;
}

int procfs_readlink_fd(int pid, int fd, char *buf, int cap)
{
    (void)pid;
    (void)fd;
    (void)buf;
    (void)cap;
    return -ENOENT;
}

fd_entry_t *fd_get(fd_table_t *table, int fd)
{
    (void)table;
    (void)fd;
    return 0;
}

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
    if (!failures) {
        printf("devfs-hosttest: result: pass: exclusive pty creation, offset devices\n");
    }
    return failures != 0;
}

/** Fail if these open rows enter an unrelated device operation. */
static int unexpected(void)
{
    check(0, "unrelated device operation");
    return -EIO;
}

uint32_t block_max_sectors(block_device_t *dev)
{
    (void)dev;
    return (uint32_t)unexpected();
}

void block_partition_init(block_partition_t *part, block_device_t *dev, uint64_t first,
                          uint64_t count)
{
    (void)part;
    (void)dev;
    (void)first;
    (void)count;
    unexpected();
}

block_device_t *block_physical(block_device_t *dev)
{
    unexpected();
    return dev;
}

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

int copy_to_user(void __user *dst, const void *src, size_t size)
{
    return copy_from_user(dst, src, size);
}

struct disk *disk_at(int index)
{
    (void)index;
    unexpected();
    return NULL;
}

int disk_count(void)
{
    return unexpected();
}

struct disk *disk_of(block_device_t *dev)
{
    (void)dev;
    unexpected();
    return NULL;
}

int disk_rescan(struct disk *d)
{
    (void)d;
    return unexpected();
}

void *kalloc_pages_try(uint32_t count, uintptr_t *phys)
{
    (void)count;
    (void)phys;
    unexpected();
    return NULL;
}

void kfree_pages(void *ptr, uint32_t count)
{
    (void)ptr;
    (void)count;
    unexpected();
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    unexpected();
}

uint32_t klog_len(void)
{
    return (uint32_t)unexpected();
}

uint32_t klog_read(char *buf, uint32_t cap, uint32_t offset)
{
    (void)buf;
    (void)cap;
    (void)offset;
    return (uint32_t)unexpected();
}

int klog_screen_level(void)
{
    return unexpected();
}

void klog_set_screen_level(int level)
{
    (void)level;
    unexpected();
}

int rtc_read_clock(rtc_time_t *out)
{
    (void)out;
    unexpected();
    return -1;
}

int rtc_write_clock(const rtc_time_t *t)
{
    (void)t;
    unexpected();
    return -1;
}

void rtc_week_and_year_day(const rtc_time_t *t, int32_t *weekday, int32_t *yearday)
{
    (void)t;
    *weekday = 0;
    *yearday = 0;
    unexpected();
}

void krandom_bytes(void *buf, uint32_t count)
{
    (void)buf;
    (void)count;
    unexpected();
}

/* The clock's alarm, its interrupt and /dev/mem, which no row reaches. */
void rtc_time_of_seconds(uint32_t seconds, rtc_time_t *t)
{
    (void)seconds;
    (void)t;
    unexpected();
}

int64_t rtc_seconds_of(const rtc_time_t *t)
{
    (void)t;
    unexpected();
    return 0;
}

void rtc_alarm_write(const rtc_time_t *t)
{
    (void)t;
    unexpected();
}

int rtc_alarm_read(rtc_time_t *t)
{
    (void)t;
    unexpected();
    return 0;
}

void rtc_alarm_interrupts(int on)
{
    (void)on;
    unexpected();
}

int rtc_alarm_ack(void)
{
    unexpected();
    return 0;
}

void suspend_wake(void)
{
    unexpected();
}

void rtc_alarm_attach(isr_handler_t handler)
{
    (void)handler;
    unexpected();
}

void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
    unexpected();
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    unexpected();
    return 0;
}

uint32_t kirq_save(void)
{
    unexpected();
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
    unexpected();
}

void *kmap_temp(uintptr_t phys)
{
    (void)phys;
    unexpected();
    return 0;
}

void kunmap_temp(void *va)
{
    (void)va;
    unexpected();
}

int paging_frame_is_device(uintptr_t phys)
{
    (void)phys;
    unexpected();
    return 0;
}

int krandom_is_trusted(void)
{
    return unexpected();
}

int loop_control_write(const char *buf, uint32_t count)
{
    (void)buf;
    (void)count;
    return unexpected();
}

/** No loop devices are attached in this fixture. */
block_device_t *loop_device_at(int index)
{
    (void)index;
    return NULL;
}

int paging_framefail_arm(uint32_t count)
{
    (void)count;
    return unexpected();
}

int pty_open_slave(int index, int flags)
{
    (void)index;
    (void)flags;
    return unexpected();
}

int pty_session_slave(int sid)
{
    (void)sid;
    return unexpected();
}

int recovery_publishes_disk(void)
{
    return unexpected();
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return unexpected();
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    unexpected();
}
