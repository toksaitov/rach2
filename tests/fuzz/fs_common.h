#ifndef _FS_COMMON_H_
#define _FS_COMMON_H_

/*
 * What the three block file system harnesses share: the kernel calls their
 * drivers make, stubbed as the host tests stub them; a block device over a
 * volume held in memory; and one walk that mounts the volume, lists up to 64
 * of its root's entries, reads each file among them and unmounts. A harness
 * fills `fs_image` with its volume and calls `fs_exercise` with its type.
 */
#include "../../rach2/sys/fs/vfs.h"
#include "../../rach2/sys/fs/bcache.h"
#include "../../rach2/sys/block/block.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/proc/process.h"

process_t proc_a;
struct percpu cpu_table[NR_CPUS] = {{&proc_a}};

void k_panic(const char *s)
{
    (void)s;
    __builtin_trap();
}

void klog(int level, const char *sys, const char *fmt, ...)
{
    (void)level;
    (void)sys;
    (void)fmt;
}

void wait_queue_init(wait_queue_t *wq)
{
    __builtin_memset(wq, 0, sizeof(*wq));
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return -1;
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}

int vfs_ns_held_by_caller(void)
{
    return 0;
}

void vfs_device_failed(block_device_t *d, uint64_t lba, int rc)
{
    (void)d;
    (void)lba;
    (void)rc;
}

void vfs_ns_lock(struct vfs_mount *mnt)
{
    mnt->ns_holder = (int)current_proc->pid;
    mnt->ns_depth++;
}

void vfs_ns_unlock(struct vfs_mount *mnt)
{
    if (mnt->ns_depth > 0) {
        mnt->ns_depth--;
    }
    if (mnt->ns_depth == 0) {
        mnt->ns_holder = -1;
    }
}

void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1000000000;
    *nanoseconds = 0;
}

/* The volume: FS_IMAGE_MAX bytes, of which `fs_image_bytes` are the device. */
#define FS_IMAGE_MAX (4u << 20)
static unsigned char fs_image[FS_IMAGE_MAX];
static uint64_t fs_image_bytes;

static int img_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    if (lba * 512 + (uint64_t)count * 512 > fs_image_bytes) {
        return -EIO;
    }
    __builtin_memcpy(buf, fs_image + lba * 512, (unsigned long)count * 512);
    return 0;
}

static int img_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    if (lba * 512 + (uint64_t)count * 512 > fs_image_bytes) {
        return -EIO;
    }
    __builtin_memcpy(fs_image + lba * 512, buf, (unsigned long)count * 512);
    return 0;
}

static block_device_t fs_dev = {512, 0, BLOCK_FAULT_SAFE, 0, 0, 0, img_read, img_write, 0, 0, 0};

/* The return code of the last mount, for a harness that reads it. */
static int fs_last_mount_rc;

int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *data)
{
    if (write) {
        return device->write(device, lba, count, data);
    }
    return device->read(device, lba, count, data);
}

/**
 * Mount the volume in `fs_image` as `type`, list up to 64 of the root's
 * entries, read up to 4 KiB of each file among them, and unmount.
 *
 * A step that returns an error ends the walk there, which is a valid result
 * for a mutated volume; what the harness is looking for is what the
 * sanitizers report.
 *
 * The block cache outlives the call, so it is invalidated first: a block the
 * last input's mount read would otherwise be returned for this input's bytes.
 */
static void fs_exercise(const struct vfs_type *type)
{
    struct vfs_mount mnt;
    const struct vfs_ops *ops = type->ops;
    void *dir;
    uint32_t cursor = 0;
    struct vfs_dirent ent;
    static unsigned char buf[4096];
    int n;

    __builtin_memset(&mnt, 0, sizeof(mnt));
    mnt.type = type;
    mnt.dev = &fs_dev;
    mnt.ns_holder = -1;
    fs_dev.sector_size = 512;
    fs_dev.sector_count = fs_image_bytes / 512;
    (void)bcache_invalidate(&fs_dev);
    fs_last_mount_rc = type->mount(&mnt, &fs_dev, 0, "");
    if (fs_last_mount_rc != 0) {
        return;
    }
    if (ops->open(&mnt, mnt.root, 0, &dir) == 0) {
        for (n = 0; n < 64 && ops->readdir(&mnt, dir, &cursor, &ent) == 1; n++) {
            void *node, *h;
            struct vfs_stat st;

            if (ops->lookup(&mnt, mnt.root, ent.name, &node) != 0) {
                continue;
            }
            (void)ops->getattr(&mnt, node, &st);
            if (!ent.is_dir && ops->open(&mnt, node, 0, &h) == 0) {
                (void)ops->read(&mnt, h, buf, sizeof(buf), 0);
                ops->release(&mnt, h);
            }
            if (ops->put) {
                ops->put(&mnt, node);
            }
        }
        ops->release(&mnt, dir);
    }
    if (ops->prepare_unmount) {
        (void)ops->prepare_unmount(&mnt);
    }
    if (ops->put) {
        ops->put(&mnt, mnt.root);
    }
    ops->unmount(&mnt);
}

#endif
