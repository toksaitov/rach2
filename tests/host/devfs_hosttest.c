/**
 * Check exclusive creation before pty or descriptor allocation.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "fs/devfs.c"

extern int printf(const char *format, ...);
static int failures, master_calls, descriptor_calls;
static process_t process;
process_t *current_proc = &process;
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

/** Check errno precedence, absent leaves and ordinary pty dispatch. */
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
    if (!failures) {
        printf("devfs-hosttest: result: pass: exclusive pty creation\n");
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

int copy_from_user(void *dst, const void *src, uint32_t size)
{
    (void)dst;
    (void)src;
    (void)size;
    return unexpected();
}

int copy_to_user(void *dst, const void *src, uint32_t size)
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

void krandom_bytes(void *buf, uint32_t count)
{
    (void)buf;
    (void)count;
    unexpected();
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
