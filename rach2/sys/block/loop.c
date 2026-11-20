/*
 * loop.c: block devices over regular files.
 *
 * A device's read is a read of the file through the file system it is on,
 * and its write is a write of that file. The device is write-through
 * (`BLOCK_WRITE_THROUGH`) and its flush is a sync of the file, so a write
 * the buffer cache sends without `BLOCK_REQ_NOFLUSH`, and every durable sync
 * of the cache, ends with the file synced, while the cache's other writes
 * leave the sync to the next flush. That is what lets a file system be in a
 * file: an image on a disk, on a stick, or on a root that is only memory.
 *
 * The device sleeps its caller and answers in the same call
 * (`BLOCK_CALLER_SYNC`): it takes no request of the pool, because the device
 * under it takes one from the same pool while it waits. It gives that device
 * as its `backing`, and the buffer cache reads the chain to let a caller
 * holding one of its blocks take a block of the device below.
 *
 * While a file is attached, other callers may write it, extend it, map it
 * shared and writable and dump into it, as on Linux, and the device reads
 * and writes the same bytes they do: a read takes resident pages first
 * (`loop_read_mapped`), and a write puts
 * its bytes in the pages mappings hold (`filemap_disk_written`). A
 * truncation and an attribute change that would change its length are
 * refused, and on KRFFS, where a change may move the file, so is every
 * change but the device's own (`loop_length_pinned`).
 *
 * The block layer calls the device without the big lock.
 * A transfer reads the attachment and counts itself in flight under
 * `loop_lock`, and a detach or a release unpublishes the device under it
 * only when no transfer is in flight. Linux's loop checks `lo_state` in
 * `loop_queue_rq` (drivers/block/loop.c:1815-1825); its `loop_clr_fd` only
 * marks the device `LO_FLAGS_AUTOCLEAR`, and `Lo_rundown` as well for a sole
 * opener (loop.c:1187-1189); with other openers the device stays `Lo_bound`
 * until `lo_release`, at the last close, marks it `Lo_rundown` and lets the
 * file go (loop.c:1707-1715), when no I/O can be in flight.
 * Here a detach does it at once and answers `EBUSY` while a transfer is in
 * flight. The transfer then runs in the caller, which is this device's
 * worker, where Linux's runs on its work queue (loop.c:776-869), and takes
 * the big lock for the file system under it as a descriptor's call does: for
 * a write and a sync, and for a read unless the type reads without it
 * (`reads_unlocked`). Attaching and detaching are system calls under the big
 * lock, which serializes them as Linux's `lo_mutex` does (loop.c:77), except
 * across a detach's sync of its buffers, which sleeps (`loop_detach`).
 */

#include "loop.h"
#include "../mm/filemap.h"

#include "../fs/bcache.h"
#include "../fs/fd.h"
#include "../fs/vfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/spinlock.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../proc/smp.h"

struct loop_device {
    block_device_t dev; /* first: a device pointer is a loop pointer */
    int used;           /* the slot is taken, whether or not it is attached */
    int attaching;      /* claimed, with the steps that can sleep still to run */
    int attached;       /* the file is open and the device can be reached */
    int index;
    uint32_t claim; /* counts the slot's claims, so a detach that slept knows its attachment */
    struct vfs_mount *mnt;
    /* The mount whose file system the file is on (`vfs_mount_base` of
     * `mnt`), which `loop_backs` compares and never dereferences. Written
     * with `node` under `loop_lock`; `mnt`'s description keeps it. */
    const struct vfs_mount *base;
    void *node;
    void *handle;
    /* The attachment's cursors into the backing mount's and inode's
     * write-back errors, reported by a flush. */
    uint32_t wb_seen, ino_seen;
    int writable;
    int in_flight; /* transfers inside the file system the file is on */
    /* The writes of the file and its syncs by the flush, which /proc/loop
     * reports so a test can see that a sync happened; the flush reads
     * `writes` to tell whether one was answered while it synced. */
    uint32_t writes;
    uint32_t syncs;
    int release_due;          /* the medium went during a transfer: the last one out releases */
    char path[LOOP_PATH_MAX]; /* what it was called when it was attached */
    uint64_t ino;             /* the file's inode number, which LOOP_GET_STATUS64 reports */
    int autoclear;            /* detach when the last description and mount have gone */
    int opens;                /* descriptions open on the node, attached or not */
};

static struct loop_device loops[LOOP_DEVICES];

/* The devices' state a transfer reads or changes, which the big lock does
 * not cover since the block layer calls a transfer without it: `attached`,
 * `in_flight`, `release_due`, `writes`, `syncs` and the device's
 * `unflushed`, `base` and `node` for `loop_backs`, and the attachment a
 * transfer reads while `attached`: the device's `sector_count` and the file
 * (`mnt`, `node`, `handle`, `ino`, `writable`), whose writers hold the big
 * lock as well, so a reader holding that sees them without this. Taken with
 * interrupts off, since a medium's going is reported from the USB pump; it
 * is a leaf. */
static struct spinlock loop_lock = SPINLOCK_INIT_NAMED("loop_lock");

/** What a transfer reads of the attachment when it begins. */
struct loop_file {
    struct vfs_mount *mnt;
    void *node;
    void *handle;
    uint64_t ino;
    int writable;
    uint32_t writes; /* `writes` as the transfer began, for the flush */
};

/** The loop device whose `dev` is `device`. */
static struct loop_device *loop_of(block_device_t *device)
{
    return (struct loop_device *)device;
}

static void loop_release(struct loop_device *loop);
static void loop_unclaim(struct loop_device *loop);
static void loop_settle(void);

/**
 * Begin a transfer of `count` sectors at `lba` of `loop`, or a flush when
 * `count` is 0: when it is attached and has those sectors, count the
 * transfer in flight and copy the attachment into `f`, under `loop_lock`.
 * Returns 0, or `-EIO` for a device not attached or a range past its end.
 *
 * The range is checked under the lock against the attachment the transfer
 * will use, since a detach and a new attach of a smaller file may come
 * between a check before it and the transfer; Linux sets the size before it
 * sets `Lo_bound` (loop_configure, loop.c:1052-1058) and checks the range
 * against it in `bio_check_eod` (block/blk-core.c:549-564). A detach or a
 * release waits for the count to fall to 0 before it lets the file go, so
 * `f` stays good until `loop_end`.
 */
static int loop_begin(struct loop_device *loop, uint64_t lba, uint32_t count, struct loop_file *f)
{
    uint32_t flags = spin_lock_irqsave(&loop_lock);

    if (!loop->attached ||
        (count != 0 && (lba >= loop->dev.sector_count || count > loop->dev.sector_count - lba))) {
        spin_unlock_irqrestore(&loop_lock, flags);
        return -EIO;
    }
    loop->in_flight++;
    f->mnt = loop->mnt;
    f->node = loop->node;
    f->handle = loop->handle;
    f->ino = loop->ino;
    f->writable = loop->writable;
    f->writes = loop->writes;
    spin_unlock_irqrestore(&loop_lock, flags);
    return 0;
}

/**
 * The device's phase (block.h): where in a sector of the medium the file's
 * first byte lies. The caller holds the file's lock through publication.
 * A file system that keeps files in whole blocks uses its device's phase.
 * The device under the file system is a disk or a partition of one, whose
 * phase is 0, or none for the boot root, whose pages begin on sectors;
 * attachment refuses a file on a loop device, even one over the boot root,
 * whose `backing` is NULL. So the file system's own cache blocks begin on
 * sectors of the medium, and the file's phase places every edge a stop can
 * cut its image at.
 */
static uint32_t loop_phase(const struct vfs_ref *ref, void *handle)
{
    struct vfs_mount *mnt = ref->mnt->bind_of ? ref->mnt->bind_of : ref->mnt;

    if (mnt->type->ops && mnt->type->ops->phase) {
        return mnt->type->ops->phase(mnt, ref->node, handle);
    }
    return mnt->dev ? mnt->dev->sector_phase : 0;
}

/**
 * End a transfer that `loop_begin` began, under `loop_lock`: a write
 * (`wrote`) counts and marks the device unflushed, a sync that succeeded
 * (`synced`) counts and clears the mark unless a write was answered while
 * it ran, since the sync may not have covered it.
 */
static void loop_end(struct loop_device *loop, const struct loop_file *f, int wrote, int synced)
{
    uint32_t flags = spin_lock_irqsave(&loop_lock);

    if (wrote) {
        loop->writes++;
        loop->dev.unflushed = 1;
    }
    if (synced) {
        loop->syncs++;
        if (loop->writes == f->writes) {
            loop->dev.unflushed = 0;
        }
    }
    loop->in_flight--;
    spin_unlock_irqrestore(&loop_lock, flags);
}

/**
 * Enter the file system under `mnt` for a transfer: count it in the
 * caller's `device_io`, so that a SIGKILL pending for the caller does not
 * end the transfer's write there, and take the big lock as a descriptor's
 * call does (`file_read_lock`, syscall_fs.c), for a read (`read`) only when
 * the type does not read without it. Returns 1 when it took the lock, which
 * `loop_fs_exit` gives back, and 0 when the caller holds it already or the
 * read needs none.
 *
 * The transfer runs in whatever thread reached the device: one that writes
 * back the cache of the file system on the device as it dies, from its exit
 * or with its process killed during a write, too. Linux's loop runs it on
 * a worker that no signal reaches (`loop_queue_work`,
 * drivers/block/loop.c:776-869), and so must this: a write refused for the
 * caller's signal is a failed write of the device, which the file system
 * above it records and answers by going read-only.
 */
static int loop_fs_enter(const struct vfs_mount *mnt, int read)
{
    if (current_proc) {
        current_proc->device_io++;
    }
    if (bkl_held() || (read && vfs_mount_base(mnt)->type->reads_unlocked)) {
        return 0;
    }
    bkl_enter();
    return 1;
}

/** Give back what `loop_fs_enter` took. */
static void loop_fs_exit(int big)
{
    if (big) {
        bkl_exit();
    }
    if (current_proc) {
        current_proc->device_io--;
    }
}

/** Whether a transfer of `loop` is inside the file system the file is on. */
static int loop_busy(struct loop_device *loop)
{
    uint32_t flags = spin_lock_irqsave(&loop_lock);
    int busy = loop->in_flight > 0;

    spin_unlock_irqrestore(&loop_lock, flags);
    return busy;
}

/**
 * Unpublish `loop`, so that no transfer begins, when none is in flight.
 * Returns 1 with `attached` clear, or 0, changing nothing, while a transfer
 * is inside the file system the file is on. The big lock is held.
 */
static int loop_quiesce(struct loop_device *loop)
{
    uint32_t flags = spin_lock_irqsave(&loop_lock);
    int idle = loop->in_flight == 0;

    if (idle) {
        loop->attached = 0;
    }
    spin_unlock_irqrestore(&loop_lock, flags);
    return idle;
}

/**
 * Unpublish `loop` for a detach that slept since it checked the device, when
 * no transfer is in flight. Returns 1 when this call cleared `attached`, or
 * when the medium under the file went meanwhile and the release it owes is
 * this caller's; 0, changing nothing, while a transfer is in flight or when
 * the device was already unpublished otherwise. The big lock is held, and
 * the caller has checked that the slot still holds the claim it began with.
 */
static int loop_unpublish(struct loop_device *loop)
{
    uint32_t flags = spin_lock_irqsave(&loop_lock);
    int ours = 0;

    if (loop->in_flight == 0 && loop->attached) {
        loop->attached = 0;
        ours = 1;
    } else if (loop->in_flight == 0 && loop->release_due) {
        ours = 1;
    }
    spin_unlock_irqrestore(&loop_lock, flags);
    return ours;
}

/**
 * Whether a transfer of `count` sectors is one this device can move: at
 * least one, and few enough that the byte count does not wrap.
 *
 * The block layer bounds every request before it calls a device, and this
 * and `loop_begin` check the same of what arrives: a device that trusted its
 * caller would read or write past the end of the file it stands on, and a
 * count that wrapped the byte arithmetic would move nothing and report
 * success.
 */
static int loop_count_ok(uint32_t count)
{
    return count != 0 && count <= 0x7FFFFFu;
}

/**
 * Read up to `len` bytes at `at` of `f`'s file into `buffer`. Read each
 * resident page, including stores awaiting write-back, before asking the
 * file system for a missing page, as Linux's `lo_read_simple` reads the
 * page cache. The caller holds the big lock. Returns a byte count or the
 * error of the attribute lookup or file system read, which may sleep.
 */
static int32_t loop_read_mapped(const struct loop_file *f, uint64_t at, void *buffer, uint32_t len)
{
    struct vfs_stat st;
    uint8_t *out = buffer;
    uint32_t done = 0;
    int32_t rc = vfs_getattr(f->mnt, f->node, &st);

    if (rc != 0 || at >= st.size) {
        return rc;
    }
    if (len > st.size - at) {
        len = (uint32_t)(st.size - at);
    }
    while (done < len) {
        uint64_t off = at + done;
        uint32_t piece = PAGE_SIZE - (uint32_t)(off % PAGE_SIZE);

        if (piece > len - done) {
            piece = len - done;
        }
        rc = (int32_t)filemap_disk_read(f->mnt, f->ino, off, out + done, piece);
        if (rc == 0) {
            rc = vfs_read(f->mnt, f->handle, out + done, piece, off);
        }
        if (rc < 0) {
            return rc;
        }
        done += (uint32_t)rc;
        if ((uint32_t)rc < piece) {
            break;
        }
    }
    return (int32_t)done;
}

/**
 * Read whole sectors from the file through the file system it is on.
 *
 * A short answer is a device error, `-EIO`: the file's length fixed the
 * sector count at the attachment and nothing may change it while it is
 * attached, so a transfer that does not move every byte means the file is not
 * what it was. The same holds for `loop_write`. A file with resident pages
 * is read page by page (`loop_read_mapped`).
 */
static int loop_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    struct loop_device *loop = loop_of(device);
    uint32_t bytes = count * BLOCK_SECTOR_SIZE;
    struct loop_file f;
    int32_t got;
    int big;

    if (!loop_count_ok(count) || loop_begin(loop, lba, count, &f) != 0) {
        return -EIO;
    }
    big = loop_fs_enter(f.mnt, 1);
    if (filemap_disk_mapped(f.mnt, f.ino)) {
        if (!bkl_held()) {
            bkl_enter();
            big = 1;
        }
        got = loop_read_mapped(&f, lba * BLOCK_SECTOR_SIZE, buffer, bytes);
    } else {
        got = vfs_read(f.mnt, f.handle, buffer, bytes, lba * BLOCK_SECTOR_SIZE);
    }
    loop_fs_exit(big);
    loop_end(loop, &f, 0, 0);
    if (got < 0 || (uint32_t)got != bytes) {
        klog(KLOG_WARN, "loop", "read: loop%d: sector %u: short by %d", loop->index, (uint32_t)lba,
             (int)(bytes - (got < 0 ? 0 : (uint32_t)got)));
        return -EIO;
    }
    return 0;
}

/**
 * Write whole sectors into the file and sync it, since the buffer cache
 * takes a written block to be on the device; `-EROFS` on a read-only
 * attachment or `-EIO` for a short write. Take the file's write lock before
 * its node lock to serialize against a whole descriptor write.
 */
static int loop_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    struct loop_device *loop = loop_of(device);
    uint32_t bytes = count * BLOCK_SECTOR_SIZE;
    struct loop_file f;
    struct vfs_ref file;
    int32_t wrote;
    int big;

    if (!loop_count_ok(count) || loop_begin(loop, lba, count, &f) != 0) {
        return -EIO;
    }
    if (!f.writable) {
        loop_end(loop, &f, 0, 0);
        return -EROFS;
    }
    big = loop_fs_enter(f.mnt, 0);
    file.mnt = f.mnt;
    file.node = f.node;
    vfs_write_lock(&file);
    vfs_node_lock(&file);
    wrote = vfs_write(f.mnt, f.handle, buffer, bytes, lba * BLOCK_SECTOR_SIZE);
    if (wrote > 0) {
        filemap_disk_written(f.mnt, f.ino, lba * BLOCK_SECTOR_SIZE, buffer, (uint32_t)wrote);
    }
    vfs_node_unlock(&file);
    vfs_write_unlock(&file);
    loop_fs_exit(big);
    loop_end(loop, &f, 1, 0);
    if (wrote < 0 || (uint32_t)wrote != bytes) {
        klog(KLOG_WARN, "loop", "write: loop%d: sector %u: short by %d", loop->index, (uint32_t)lba,
             (int)(bytes - (wrote < 0 ? 0 : (uint32_t)wrote)));
        return -EIO;
    }
    return 0;
}

/**
 * Write back the file's mapped pages and sync its data and metadata, as
 * Linux's `lo_req_flush` does. Returns -EIO on a write-back or sync error.
 * `request` is always NULL for this device. Clear the unflushed mark only
 * when no device write was answered while the sync slept.
 */
static int loop_flush(block_device_t *device, struct block_request *request)
{
    struct loop_device *loop = loop_of(device);
    struct loop_file f;
    int big, mapped, rc, kept;

    (void)request;
    if (loop_begin(loop, 0, 0, &f) != 0) {
        return -EIO;
    }
    big = loop_fs_enter(f.mnt, 0);
    mapped = filemap_sync_inode(f.mnt, f.ino);
    rc = vfs_fsync(f.mnt, f.handle, &loop->wb_seen);
    kept = vfs_ino_err_check(f.mnt, f.ino, &loop->ino_seen);
    if (rc == 0) {
        rc = mapped != 0 ? mapped : kept;
    }
    loop_fs_exit(big);
    loop_end(loop, &f, 0, rc == 0);
    return rc == 0 ? 0 : -EIO;
}

block_device_t *loop_attached_at(int index)
{
    if (index < 0 || index >= LOOP_DEVICES || !loops[index].attached) {
        return 0;
    }
    return &loops[index].dev;
}

block_device_t *loop_node_device(int index, int write, int32_t *err)
{
    block_device_t *dev = loop_attached_at(index);

    *err = !dev                              ? -ENXIO
           : write && !loops[index].writable ? -EROFS
           : write && vfs_device_holds(dev)  ? -EBUSY
                                             : 0;
    return *err == 0 ? dev : 0;
}

/** Drain a loop node's buffers under one hold of its attachment. */
int loop_node_sync(int index)
{
    struct loop_device *loop;
    struct loop_file f;
    int rc;

    if (index < 0 || index >= LOOP_DEVICES) {
        return -ENXIO;
    }
    loop = &loops[index];
    if (loop_begin(loop, 0, 0, &f) != 0) {
        return -ENXIO;
    }
    rc = bcache_sync(&loop->dev);
    loop_end(loop, &f, 0, 0);
    return rc;
}

/**
 * Follow a node that moved, which is what a KRFFS relocation does to one.
 *
 * An attachment names its file by its mount and its node, and the VFS owns
 * the sweep over every other holder; this is the loop devices' part of it.
 * Only the memory root uses node pointers as open handles. Mounted KRFFS
 * retains its open record when the node moves.
 */
void loop_repoint(struct vfs_mount *mnt, const void *from, void *to)
{
    uint32_t flags;
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (loops[i].mnt != mnt) {
            continue;
        }
        flags = spin_lock_irqsave(&loop_lock);
        if (loops[i].node == from) {
            loops[i].node = to;
        }
        if (mnt->type == &vfs_krffs_type && loops[i].handle == from) {
            loops[i].handle = to;
        }
        spin_unlock_irqrestore(&loop_lock, flags);
    }
}

/**
 * Whether the file `node` of `mnt` backs a loop device. A caller with an
 * open handle asks with its node (`handle_node` in vfs.c), since another
 * open of the same file has a handle of its own.
 *
 * The mounts are compared by the file system they show (`vfs_mount_base`):
 * a bind keeps the nodes of the mount it was made from under a record of its
 * own, so a file attached through one mount and reached through another is
 * the same file. The slot's side is the base the attach cached, read with
 * the node under `loop_lock`, since a detach clears them without the lock a
 * caller of this may hold, and the slot's own mount, a bind that the detach
 * may let the reaper free, is never read.
 */
int loop_backs(struct vfs_mount *mnt, void *node)
{
    const struct vfs_mount *base = vfs_mount_base(mnt);
    uint32_t flags = spin_lock_irqsave(&loop_lock);
    int i, backs = 0;

    for (i = 0; i < LOOP_DEVICES && !backs; i++) {
        backs = loops[i].base == base && loops[i].node == node;
    }
    spin_unlock_irqrestore(&loop_lock, flags);
    return backs;
}

/**
 * Whether the length of `node` of `mnt` may not change while it backs a
 * loop device. A type with a `phase` places a file's first byte anywhere in
 * a sector, and a change of length may move the file (KRFFS's relocation),
 * which would change the phase the device gave the volume on it at the
 * attach: that volume keeps each field it publishes inside one sector of
 * that phase, so that a stop cannot tear it. Every other type keeps a file
 * where it is, in whole blocks, and Linux's loop reads past the new end as
 * zeros and writes there by extending the file (`loop_read`).
 */
int loop_length_pinned(struct vfs_mount *mnt, void *node)
{
    const struct vfs_mount *base = vfs_mount_base(mnt);

    return base->type->ops && base->type->ops->phase && loop_backs(mnt, node);
}

/**
 * Let go of everything the attachment holds and free the slot. The buffers
 * go first: a device whose file is released while the cache still has its
 * blocks would write them into a file nobody holds open.
 *
 * Never called while a transfer is inside the file system the file is on:
 * the callers unpublish the device first (`loop_quiesce`), which no
 * transfer can begin on.
 */
static void loop_release(struct loop_device *loop)
{
    struct vfs_mount *mnt = loop->mnt;
    void *node = loop->node;
    void *handle = loop->handle;
    uint32_t flags = spin_lock_irqsave(&loop_lock);

    loop->attached = 0;
    loop->release_due = 0;
    loop->base = 0;
    loop->node = 0;
    spin_unlock_irqrestore(&loop_lock, flags);
    loop->attaching = 0;
    loop->used = 0;
    loop->autoclear = 0;
    loop->mnt = 0;
    loop->handle = 0;
    loop->path[0] = '\0';
    bcache_invalidate(&loop->dev);
    if (mnt) {
        vfs_desc_close(mnt, node, handle);
    }
}

/**
 * Give back a claim whose attachment never finished. The slot is free again
 * and nothing was published into it, so there is nothing to let go of.
 */
static void loop_unclaim(struct loop_device *loop)
{
    loop->attaching = 0;
    loop->used = 0;
}

/**
 * The release a medium's going left owing, done where it can be done.
 *
 * A device that goes is reported from wherever the driver noticed, and
 * letting go of what the attachment holds takes a mount's lock and can sleep.
 * So the going only marks the device, and this releases each marked device
 * with no transfer in flight that nothing holds. It runs from system calls:
 * the control node's commands and ioctls, a device's ioctls, and the reads of
 * /proc/loop and /sys/block/loopN/loop.
 */
static void loop_settle(void)
{
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        struct loop_device *loop = &loops[i];

        if (!loop->release_due || vfs_device_holds(&loop->dev) || !loop_quiesce(loop)) {
            continue;
        }
        loop_release(loop);
    }
}

void loop_device_gone(block_device_t *device)
{
    uint32_t flags;
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (!loops[i].used || loops[i].dev.backing != device) {
            continue;
        }
        klog(KLOG_WARN, "loop", "loop%d: the device under its file has gone", i);
        flags = spin_lock_irqsave(&loop_lock);
        loops[i].dev.flags |= BLOCK_GONE;
        loops[i].attached = 0;
        loops[i].release_due = 1;
        spin_unlock_irqrestore(&loop_lock, flags);
        bcache_device_gone(&loops[i].dev);
    }
}

/** Append what fits of `text` to `buffer` at `at`, leaving room for a NUL,
 * and return where the next byte goes. */
static uint32_t put_text(char *buffer, uint32_t at, uint32_t cap, const char *text)
{
    while (*text && at + 1 < cap) {
        buffer[at++] = *text++;
    }
    return at;
}

/** Append a number in decimal the same way, by subtracting powers of ten,
 * since the i686 kernel has no 64-bit division. */
static uint32_t put_number(char *buffer, uint32_t at, uint32_t cap, uint64_t value)
{
    uint64_t ten = 1;
    uint32_t n = 1, i;
    char digit;

    while (n < 20u && ten * 10u <= value) {
        ten *= 10u;
        n++;
    }
    while (n > 0) {
        for (digit = '0'; value >= ten; digit++) {
            value -= ten;
        }
        if (at + 1 < cap) {
            buffer[at++] = digit;
        }
        n--;
        ten = 1;
        for (i = 1; i < n; i++) {
            ten *= 10u;
        }
    }
    return at;
}

int loop_line(int index, char *buffer, uint32_t cap)
{
    struct loop_device *loop;
    uint32_t at = 0, writes, syncs, flags;

    loop_settle();
    if (index < 0 || index >= LOOP_DEVICES || !loops[index].used || cap == 0) {
        return 0;
    }
    loop = &loops[index];
    flags = spin_lock_irqsave(&loop_lock);
    writes = loop->writes;
    syncs = loop->syncs;
    spin_unlock_irqrestore(&loop_lock, flags);
    at = put_text(buffer, at, cap, "loop");
    at = put_number(buffer, at, cap, (uint32_t)index);
    at = put_text(buffer, at, cap, " ");
    at = put_number(buffer, at, cap, loop->dev.sector_count * BLOCK_SECTOR_SIZE);
    at = put_text(buffer, at, cap, loop->writable ? " rw " : " ro ");
    at = put_text(buffer, at, cap, "writes ");
    at = put_number(buffer, at, cap, writes);
    at = put_text(buffer, at, cap, " syncs ");
    at = put_number(buffer, at, cap, syncs);
    at = put_text(buffer, at, cap, " ");
    at = put_text(buffer, at, cap, loop->attached ? loop->path : "(gone)");
    at = put_text(buffer, at, cap, "\n");
    buffer[at] = '\0';
    return (int)at;
}

const char *loop_backing(int index, int *autoclear)
{
    loop_settle();
    if (index < 0 || index >= LOOP_DEVICES || !loops[index].used || !loops[index].attached) {
        return NULL;
    }
    *autoclear = loops[index].autoclear;
    return loops[index].path;
}

/**
 * Resolve the backing path again after acquiring its mount's namespace lock.
 *
 * The first reference keeps the mount active while the lock waits. Its node
 * may move during that wait, so only the second resolution supplies the node
 * to open. A path moved to another mount answers EBUSY. Success keeps the
 * lock through open and publication. On a type that sets `file_locked`,
 * whose nodes do not move, the write and node locks of the file are taken
 * on the first resolution's node. The write lock excludes descriptor
 * writes that started before publication and may still fault on their
 * sources. The caller gives the locks back with `loop_resolve_unlock`.
 */
static int loop_resolve(const char *path, struct vfs_ref *ref)
{
    struct vfs_ref first;
    struct vfs_mount *mnt;
    int rc;

    rc = vfs_resolve(0, path, 0, &first, 0, 0);
    if (rc != 0) {
        return rc;
    }
    if (vfs_file_locked(first.mnt)) {
        *ref = first;
        vfs_write_lock(ref);
        vfs_node_lock(ref);
        return 0;
    }
    mnt = first.mnt;
    vfs_ns_lock(mnt);
    rc = vfs_resolve(0, path, 0, ref, 0, 0);
    if (rc == 0 && ref->mnt != mnt) {
        vfs_ref_release(ref);
        rc = -EBUSY;
    }
    if (rc != 0) {
        vfs_ns_unlock(mnt);
    }
    vfs_ref_release(&first);
    return rc;
}

/** Release the file or namespace locks that `loop_resolve` took. */
static void loop_resolve_unlock(const struct vfs_ref *ref)
{
    vfs_node_unlock(ref);
    vfs_write_unlock(ref);
}

/**
 * Sync the file just opened as `handle`, through the attachment's cursor
 * `seen` into its file system's write-back errors, then check again, under
 * the mount's namespace lock, that its length is whole sectors, that it is
 * not attached, and, on a type whose change may move the file
 * (`loop_length_pinned`), that no write or truncation is in flight on it.
 * 0, or `-EINVAL` or `-EBUSY`, or what the sync or the attribute read
 * answered. A shared mapping of the file does not refuse it, as Linux's
 * does not: the device reads the mapping's stores (`loop_read_mapped`).
 *
 * Resolution and open run under the same lock, which the caller keeps
 * through publication on success, so no writer can relocate the node between
 * these steps.
 */
static int loop_prepare(struct vfs_ref *ref, void *handle, struct vfs_stat *st, uint32_t *seen)
{
    int rc = vfs_fsync(ref->mnt, handle, seen);

    if (rc == 0) {
        rc = vfs_getattr(ref->mnt, ref->node, st);
    }
    if (rc == 0 && (st->size == 0 || st->size % BLOCK_SECTOR_SIZE != 0)) {
        rc = -EINVAL;
    }
    if (rc == 0 &&
        (loop_backs(ref->mnt, ref->node) ||
         (vfs_mount_base(ref->mnt)->type->ops && vfs_mount_base(ref->mnt)->type->ops->phase &&
          fd_node_busy(ref->mnt, ref->node)))) {
        rc = -EBUSY;
    }
    return rc;
}

/**
 * Fill in `loop`'s device over the file `node` of `loop->mnt`, `size` bytes
 * on the device `backing`, and publish it under `loop_lock`, after every
 * field a transfer reads is set. `phase` is where the first byte lies in
 * a sector of the medium. Publish the file's identity with it.
 */
static void loop_publish(struct loop_device *loop, void *node, block_device_t *backing,
                         uint64_t size, uint32_t phase)
{
    uint32_t flags;

    loop->dev.sector_size = BLOCK_SECTOR_SIZE;
    loop->dev.sector_count = size / BLOCK_SECTOR_SIZE;
    loop->dev.flags =
        BLOCK_CALLER_SYNC | BLOCK_WRITE_THROUGH | (loop->writable ? 0u : BLOCK_READ_ONLY);
    if (!loop->mnt->type->ops->phase) {
        loop->dev.flags |= BLOCK_EXTERNAL_WRITES;
    }
    loop->dev.unflushed = 0;
    loop->dev.deadline_ticks = 0;
    loop->dev.read = loop_read;
    loop->dev.write = loop_write;
    loop->dev.submit = 0;
    loop->dev.flush = loop_flush;
    loop->dev.sector_phase = phase;
    loop->dev.backing = backing;
    loop->dev.drvdata = loop;
    flags = spin_lock_irqsave(&loop_lock);
    loop->base = vfs_mount_base(loop->mnt);
    loop->node = node;
    loop->attached = 1;
    spin_unlock_irqrestore(&loop_lock, flags);
}

/**
 * The slot is claimed before the steps that can sleep: a second attach that
 * found it free would publish over this one, and what the first had opened
 * would have nobody left to release it. The claim forgets the device the
 * slot last stood on, so a medium going while those steps sleep does not
 * match a claim that has published nothing.
 *
 * The description keeps the mount, so unmounting the volume the file is on
 * answers `EBUSY` while the device is attached. The resolution's count of an
 * operation in progress is given back, as an open gives it back: what the
 * attachment keeps is the hold and the description, released at the detach.
 */
int loop_attach(int index, const char *path, int writable)
{
    struct vfs_ref ref;
    struct vfs_stat st;
    struct loop_device *loop = 0;
    void *handle = 0;
    uint32_t seen, seen_fs; /* the attachment's cursors; syncfs's is unused */
    int i;
    int rc;

    if (index >= LOOP_DEVICES) {
        return -EINVAL;
    }
    if (index < 0) {
        for (i = 0; i < LOOP_DEVICES; i++) {
            if (!loops[i].used) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            return -EBUSY;
        }
    }
    loop_settle();
    loop = &loops[index];
    if (loop->used) {
        return -EBUSY;
    }
    loop->used = 1;
    loop->claim++;
    loop->attaching = 1;
    loop->attached = 0;
    loop->in_flight = 0;
    loop->writes = 0;
    loop->syncs = 0;
    loop->release_due = 0;
    loop->autoclear = 0;
    loop->mnt = 0;
    loop->handle = 0;
    loop->dev.backing = 0;

    rc = loop_resolve(path, &ref);
    if (rc != 0) {
        loop_unclaim(loop);
        return rc;
    }
    rc = vfs_getattr(ref.mnt, ref.node, &st);
    if (rc != 0) {
        loop_resolve_unlock(&ref);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return rc;
    }
    if ((st.mode & 0170000u) != 0100000u) {
        loop_resolve_unlock(&ref);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EINVAL; /* only a regular file is an image */
    }
    if (st.size == 0 || st.size % BLOCK_SECTOR_SIZE != 0) {
        loop_resolve_unlock(&ref);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EINVAL; /* whole sectors only */
    }
    if (loop_backs(ref.mnt, ref.node)) {
        loop_resolve_unlock(&ref);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EBUSY;
    }
    if ((ref.mnt->dev && ref.mnt->dev->read == loop_read) ||
        block_depth(ref.mnt->dev) + 1 > BLOCK_MAX_DEPTH) {
        loop_resolve_unlock(&ref);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -ELOOP; /* on a loop device, or deeper than the buffer cache serves */
    }

    rc = vfs_open_node(ref.mnt, ref.node, writable ? 2 : 0, &handle);
    if (rc != 0) {
        loop_resolve_unlock(&ref);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return rc;
    }
    vfs_wb_err_sample(ref.mnt, &seen, &seen_fs);
    rc = loop_prepare(&ref, handle, &st, &seen);
    if (rc != 0) {
        vfs_release(ref.mnt, handle);
        loop_resolve_unlock(&ref);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return rc;
    }

    vfs_desc_open(ref.mnt);
    vfs_leave(ref.mnt);
    loop->index = index;
    loop->mnt = ref.mnt;
    loop->handle = handle;
    loop->wb_seen = seen;
    loop->ino_seen = vfs_ino_err_sample(ref.mnt, st.ino);
    loop->writable = writable ? 1 : 0;
    loop->ino = st.ino;
    k_strncpy(loop->path, path, sizeof(loop->path));
    loop->path[sizeof(loop->path) - 1] = '\0';
    loop->attaching = 0;
    loop_publish(loop, ref.node, ref.mnt->dev, st.size, loop_phase(&ref, handle));

    loop_resolve_unlock(&ref);

    klog(KLOG_INFO, "loop", "loop%d: %s, %u sectors, %s", index, loop->path,
         (uint32_t)(st.size / BLOCK_SECTOR_SIZE), loop->writable ? "read-write" : "read-only");
    return index;
}

void loop_rename(int index, const char *path)
{
    struct loop_device *loop;

    if (index < 0 || index >= LOOP_DEVICES || !loops[index].attached) {
        return;
    }
    loop = &loops[index];
    k_strncpy(loop->path, path, sizeof(loop->path));
    loop->path[sizeof(loop->path) - 1] = '\0';
}

/**
 * A slot claimed by an attachment still inside the steps that sleep is
 * refused: releasing it would free a claim about to publish itself, and the
 * next attach would then stand a second file under it.
 *
 * The sync of the device's buffers sleeps, and lets the big lock go. A
 * second detach may finish meanwhile and a new attach claim the slot, so
 * after the sync the detach goes on only while the slot holds the claim it
 * began with, and answers `ENXIO` otherwise, as Linux's `loop_clr_fd`
 * answers a device no longer `Lo_bound` (drivers/block/loop.c:1177-1180).
 * A mount made while it slept answers `EBUSY`.
 */
int loop_detach(int index)
{
    struct loop_device *loop;
    uint32_t claim;
    int rc;

    if (index < 0 || index >= LOOP_DEVICES) {
        return -EINVAL;
    }
    loop = &loops[index];
    if (!loop->used) {
        return -ENOENT;
    }
    if (loop->attaching) {
        return -EBUSY;
    }
    if (!loop->attached) {
        if (vfs_device_holds(&loop->dev) || !loop_quiesce(loop)) {
            return -EBUSY;
        }
        klog(KLOG_INFO, "loop", "loop%d: detached", index);
        loop_release(loop);
        return 0;
    }
    if (vfs_device_holds(&loop->dev)) {
        return -EBUSY; /* something is mounted on it */
    }
    if (bcache_device_holds(&loop->dev)) {
        return -EBUSY; /* a buffer of it is held */
    }
    if (loop_busy(loop)) {
        return -EBUSY; /* a transfer is inside the file system the file is on */
    }
    claim = loop->claim;
    rc = bcache_sync(&loop->dev);
    if (!loop->used || loop->attaching || loop->claim != claim) {
        return -ENXIO; /* another detach let it go while the sync slept */
    }
    if (rc != 0) {
        return -EIO;
    }
    if (vfs_device_holds(&loop->dev)) {
        return -EBUSY;
    }
    if (bcache_invalidate(&loop->dev) != 0) {
        return -EBUSY;
    }
    if (!loop_unpublish(loop)) {
        return -EBUSY; /* a transfer began while the sync slept */
    }
    klog(KLOG_INFO, "loop", "loop%d: detached", index);
    loop_release(loop);
    return 0;
}

/**
 * The device number at `*at`, and `*at` moved past it.
 *
 * A number and nothing else: no digits, a number beyond the devices there
 * are, or anything but a space or the end after it, and the answer is -1.
 */
static int read_index(char **at)
{
    char *p = *at;
    int index = 0;
    int digits = 0;

    while (*p >= '0' && *p <= '9') {
        if (index > LOOP_DEVICES) {
            return -1;
        }
        index = index * 10 + (*p - '0');
        digits++;
        p++;
    }
    if (digits == 0 || index >= LOOP_DEVICES || (*p != ' ' && *p != '\0')) {
        return -1;
    }
    *at = p;
    return index;
}

/** Whether `line` begins with `word`. */
static int word_is(const char *line, const char *word)
{
    uint32_t i;

    for (i = 0; word[i] != '\0'; i++) {
        if (line[i] != word[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * A line and not a structure, because a line is what a shell can send.
 */
int loop_control_write(const char *buffer, uint32_t count)
{
    char line[LOOP_PATH_MAX + 16];
    char *word;
    int index = -1;
    uint32_t i;

    loop_settle();
    if (count == 0 || count >= sizeof(line)) {
        return -EINVAL;
    }
    for (i = 0; i < count; i++) {
        line[i] = buffer[i];
    }
    line[count] = '\0';
    while (count > 0 && (line[count - 1] == '\n' || line[count - 1] == ' ')) {
        line[--count] = '\0';
    }

    word = line;
    while (*word == ' ') {
        word++;
    }
    if (word_is(word, "attach ")) {
        char *arg = word + 7;
        char *path;
        int writable = 1;

        while (*arg == ' ') {
            arg++;
        }
        if (word_is(arg, "ro ")) {
            writable = 0;
            arg += 3;
            while (*arg == ' ') {
                arg++;
            }
        }
        if (*arg == '-' && arg[1] == ' ') {
            arg++;
        } else {
            index = read_index(&arg);
            if (index < 0) {
                return -EINVAL;
            }
        }
        while (*arg == ' ') {
            arg++;
        }
        path = arg;
        if (*path != '/') {
            return -EINVAL; /* an absolute path, since the device has no cwd */
        }
        return loop_attach(index, path, writable);
    }
    if (word_is(word, "detach ")) {
        char *arg = word + 7;

        while (*arg == ' ') {
            arg++;
        }
        index = read_index(&arg);
        if (index < 0 || *arg != '\0') {
            return -EINVAL; /* `detach N` and nothing after it */
        }
        return loop_detach(index);
    }
    return -EINVAL;
}

/* struct loop_info64: its size and the offsets of the fields this driver
 * reads or fills. */
#define INFO_SIZE      232u
#define INFO_INODE     8u
#define INFO_OFFSET    24u
#define INFO_SIZELIMIT 32u
#define INFO_NUMBER    40u
#define INFO_ENCRYPT   44u
#define INFO_FLAGS     52u
#define INFO_NAME      56u
#define INFO_NAME_SIZE 64u

int loop_info_flags(const uint8_t *info, uint32_t *flags)
{
    uint32_t encrypt, i;

    k_memcpy(&encrypt, info + INFO_ENCRYPT, 4);
    k_memcpy(flags, info + INFO_FLAGS, 4);
    for (i = INFO_OFFSET; i < INFO_SIZELIMIT + 8; i++) {
        if (info[i] != 0) {
            return -EINVAL; /* an offset or a size limit */
        }
    }
    return encrypt != 0 ? -EINVAL : 0;
}

int loop_attach_file(int index, const char *path, int writable, uint32_t flags)
{
    int rc = loop_attach(index, path, writable && !(flags & LO_FLAGS_READ_ONLY));

    if (rc < 0) {
        return rc;
    }
    loops[rc].autoclear = (flags & LO_FLAGS_AUTOCLEAR) != 0;
    return 0;
}

/** Detach `loop` when it is set to clear itself and nothing holds it: no
 * description of its node and no mount. */
static void loop_autoclear(struct loop_device *loop)
{
    if (loop->autoclear && loop->attached && loop->opens == 0 && !vfs_device_holds(&loop->dev)) {
        (void)loop_detach(loop->index);
    }
}

/** Fill the struct loop_info64 `info` with device `loop`'s attachment. */
static void loop_info_fill(const struct loop_device *loop, uint8_t *info)
{
    uint32_t number = (uint32_t)loop->index;
    uint32_t flags =
        (loop->writable ? 0u : LO_FLAGS_READ_ONLY) | (loop->autoclear ? LO_FLAGS_AUTOCLEAR : 0u);

    k_memset(info, 0, INFO_SIZE);
    k_memcpy(info + INFO_INODE, &loop->ino, sizeof(loop->ino));
    k_memcpy(info + INFO_NUMBER, &number, 4);
    k_memcpy(info + INFO_FLAGS, &flags, 4);
    k_strncpy((char *)info + INFO_NAME, loop->path, INFO_NAME_SIZE - 1);
}

/**
 * The file's length fixed the capacity at the attachment, and the device
 * reads through the file system's cache in 512-byte blocks, so
 * LOOP_SET_CAPACITY succeeds and changes nothing, and LOOP_SET_DIRECT_IO and
 * LOOP_SET_BLOCK_SIZE accept only what the device already does.
 */
int32_t loop_ioctl(int index, int privileged, uint32_t request, uintptr_t arg)
{
    struct loop_device *loop = &loops[index];
    uint8_t info[INFO_SIZE];
    uint32_t flags;

    loop_settle();
    if (request == LOOP_SET_CAPACITY || request == LOOP_SET_DIRECT_IO ||
        request == LOOP_SET_BLOCK_SIZE) {
        if (!loop->attached) {
            return -ENXIO;
        }
        return (request == LOOP_SET_DIRECT_IO && arg != 0) ||
                       (request == LOOP_SET_BLOCK_SIZE && arg != BLOCK_SECTOR_SIZE)
                   ? -EINVAL
                   : 0;
    }
    if (request != LOOP_CLR_FD && request != LOOP_SET_STATUS64 && request != LOOP_GET_STATUS64) {
        return -ENOTTY;
    }
    if (!loop->attached) {
        return -ENXIO;
    }
    if (request == LOOP_GET_STATUS64) {
        loop_info_fill(loop, info);
        return copy_to_user((void __user *)arg, info, INFO_SIZE) != 0 ? -EFAULT : 0;
    }
    if (!privileged) {
        return -EPERM;
    }
    if (request == LOOP_CLR_FD) {
        if (vfs_device_holds(&loop->dev) || loop->opens > 1) {
            loop->autoclear = 1;
            return 0;
        }
        return loop_detach(index);
    }
    if (copy_from_user(info, (const void __user *)arg, INFO_SIZE) != 0) {
        return -EFAULT;
    }
    if (loop_info_flags(info, &flags) != 0) {
        return -EINVAL;
    }
    loop->autoclear = (flags & LO_FLAGS_AUTOCLEAR) != 0;
    return 0;
}

int32_t loop_control_ioctl(uint32_t request, uintptr_t arg)
{
    int i;

    loop_settle();
    if (request == LOOP_CTL_GET_FREE) {
        for (i = 0; i < LOOP_DEVICES; i++) {
            if (!loops[i].used) {
                return i;
            }
        }
        return -ENOSPC;
    }
    if (request == LOOP_CTL_ADD) {
        return arg < LOOP_DEVICES ? -EEXIST : -ENOSPC; /* the four are made at boot */
    }
    if (request == LOOP_CTL_REMOVE) {
        return arg < LOOP_DEVICES ? -EBUSY : -ENODEV;
    }
    return -ENOTTY;
}

void loop_node_opened(int index)
{
    loops[index].opens++;
}

void loop_node_closed(int index)
{
    loops[index].opens--;
    loop_autoclear(&loops[index]);
}

void loop_unmounted(block_device_t *device)
{
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (device == &loops[i].dev) {
            loop_autoclear(&loops[i]);
        }
    }
}
