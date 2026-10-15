/*
 * loop.c: block devices over regular files.
 *
 * A device's read is a read of the file through the file system it is on,
 * and its write is a write followed by a sync of that file, so a block the
 * buffer cache has written is in the file. That is what lets a file system
 * be in a file: an image on a disk, on a stick, or on a root that is only
 * memory.
 *
 * The device sleeps its caller and answers in the same call
 * (`BLOCK_CALLER_SYNC`): it takes no request of the pool, because the device
 * under it takes one from the same pool while it waits. It gives that device
 * as its `backing`, and the buffer cache reads the chain to let a caller
 * holding one of its blocks take a block of the device below.
 *
 * While a file is attached it is reached through the device and nowhere
 * else: a write, a truncation and an attribute change that would change its
 * length are refused, so the image does not move under the file system
 * mounted on the device.
 */

#include "loop.h"
#include "../mm/filemap.h"

#include "../fs/bcache.h"
#include "../fs/fd.h"
#include "../fs/vfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"

struct loop_device {
    block_device_t dev; /* first: a device pointer is a loop pointer */
    int used;           /* the slot is taken, whether or not it is attached */
    int attaching;      /* claimed, with the steps that can sleep still to run */
    int attached;       /* the file is open and the device can be reached */
    int index;
    struct vfs_mount *mnt;
    void *node;
    void *handle;
    int writable;
    int in_flight; /* transfers inside the file system the file is on */
    /* Every write is a write of the file and a sync of it, and /proc/loop
     * reports both counts so a test can see that the sync happened. */
    uint32_t writes;
    uint32_t syncs;
    int release_due;          /* the medium went during a transfer: the last one out releases */
    char path[LOOP_PATH_MAX]; /* what it was called when it was attached */
    uint32_t ino;             /* the file's inode number, which LOOP_GET_STATUS64 reports */
    int autoclear;            /* detach when the last description and mount have gone */
    int opens;                /* descriptions open on the node, attached or not */
};

static struct loop_device loops[LOOP_DEVICES];

/** The loop device whose `dev` is `device`. */
static struct loop_device *loop_of(block_device_t *device)
{
    return (struct loop_device *)device;
}

static void loop_release(struct loop_device *loop);
static void loop_unclaim(struct loop_device *loop);
static void loop_transfer_ended(struct loop_device *loop);
static void loop_settle(void);

/**
 * Whether a transfer of `count` sectors at `lba` is one this device has.
 *
 * The block layer bounds every request before it calls a device, and this
 * checks the same of what arrives: a device that trusted its caller would
 * read or write past the end of the file it stands on, and a count that
 * wrapped the byte arithmetic would move nothing and report success.
 */
static int loop_bounded(struct loop_device *loop, uint64_t lba, uint32_t count)
{
    if (count == 0 || count > 0x7FFFFFu) {
        return 0;
    }
    if (lba >= loop->dev.sector_count || count > loop->dev.sector_count - lba) {
        return 0;
    }
    return 1;
}

/**
 * Read whole sectors from the file through the file system it is on.
 *
 * A short answer is a device error, `-EIO`: the file's length fixed the
 * sector count at the attachment and nothing may change it while it is
 * attached, so a transfer that does not move every byte means the file is not
 * what it was. The same holds for `loop_write`.
 */
static int loop_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    struct loop_device *loop = loop_of(device);
    uint32_t bytes = count * BLOCK_SECTOR_SIZE;
    int32_t got;

    if (!loop->attached || !loop_bounded(loop, lba, count)) {
        return -EIO;
    }
    loop->in_flight++;
    got = vfs_read(loop->mnt, loop->handle, buffer, bytes, lba * BLOCK_SECTOR_SIZE);
    loop_transfer_ended(loop);
    if (got < 0 || (uint32_t)got != bytes) {
        klog(KLOG_WARN, "loop", "read: loop%d: sector %d: short by %d", loop->index, (int)lba,
             (int)(bytes - (got < 0 ? 0 : (uint32_t)got)));
        return -EIO;
    }
    return 0;
}

/**
 * Write whole sectors into the file and sync it, since the buffer cache
 * takes a written block to be on the device; `-EROFS` on a read-only
 * attachment, `-EIO` for a short write or a failed sync.
 */
static int loop_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    struct loop_device *loop = loop_of(device);
    uint32_t bytes = count * BLOCK_SECTOR_SIZE;
    int32_t wrote;

    if (!loop->attached || !loop_bounded(loop, lba, count)) {
        return -EIO;
    }
    if (!loop->writable) {
        return -EROFS;
    }
    loop->in_flight++;
    wrote = vfs_write(loop->mnt, loop->handle, buffer, bytes, lba * BLOCK_SECTOR_SIZE);
    loop->writes++;
    if (wrote >= 0 && (uint32_t)wrote == bytes) {
        if (vfs_fsync(loop->mnt, loop->handle) != 0) {
            loop_transfer_ended(loop);
            return -EIO;
        }
        loop->syncs++;
    }
    loop_transfer_ended(loop);
    if (wrote < 0 || (uint32_t)wrote != bytes) {
        klog(KLOG_WARN, "loop", "write: loop%d: sector %d: short by %d", loop->index, (int)lba,
             (int)(bytes - (wrote < 0 ? 0 : (uint32_t)wrote)));
        return -EIO;
    }
    return 0;
}

/** Sync the file, in the call; `request` is always NULL for this device. */
static int loop_flush(block_device_t *device, struct block_request *request)
{
    struct loop_device *loop = loop_of(device);
    int rc;

    (void)request;
    if (!loop->attached) {
        return -EIO;
    }
    loop->in_flight++;
    rc = vfs_fsync(loop->mnt, loop->handle);
    loop_transfer_ended(loop);
    return rc == 0 ? 0 : -EIO;
}

block_device_t *loop_device_at(int index)
{
    if (index < 0 || index >= LOOP_DEVICES || !loops[index].used) {
        return 0;
    }
    return &loops[index].dev;
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
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (loops[i].mnt != mnt) {
            continue;
        }
        if (loops[i].node == from) {
            loops[i].node = to;
        }
        if (mnt->type == &vfs_krffs_type && loops[i].handle == from) {
            loops[i].handle = to;
        }
    }
}

/**
 * A file goes by either name: callers that have a node ask with a node, and
 * callers that have a description ask with the handle the file system gave
 * its open.
 */
int loop_backs(struct vfs_mount *mnt, void *node)
{
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (loops[i].mnt != mnt) {
            continue;
        }
        if (loops[i].node == node || loops[i].handle == node) {
            return 1;
        }
    }
    return 0;
}

/**
 * Let go of everything the attachment holds and free the slot. The buffers
 * go first: a device whose file is released while the cache still has its
 * blocks would write them into a file nobody holds open.
 *
 * Never called while a transfer is inside the file system the file is on:
 * the callers check `in_flight`.
 */
static void loop_release(struct loop_device *loop)
{
    struct vfs_mount *mnt = loop->mnt;
    void *node = loop->node;
    void *handle = loop->handle;

    loop->attached = 0;
    loop->attaching = 0;
    loop->used = 0;
    loop->release_due = 0;
    loop->autoclear = 0;
    loop->mnt = 0;
    loop->node = 0;
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

/** The end of one transfer inside the file system the file is on. */
static void loop_transfer_ended(struct loop_device *loop)
{
    loop->in_flight--;
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

        if (!loop->release_due || loop->in_flight > 0) {
            continue;
        }
        if (vfs_device_holds(&loop->dev)) {
            continue;
        }
        loop_release(loop);
    }
}

void loop_device_gone(block_device_t *device)
{
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (!loops[i].used || loops[i].dev.backing != device) {
            continue;
        }
        klog(KLOG_WARN, "loop", "loop%d: the device under its file has gone", i);
        loops[i].dev.flags |= BLOCK_GONE;
        loops[i].attached = 0;
        loops[i].release_due = 1;
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

/** Append a 32-bit number the same way, since the kernel has no 64-bit
 * division. */
static uint32_t put_number(char *buffer, uint32_t at, uint32_t cap, uint32_t value)
{
    char digits[24];
    int i = 0;

    do {
        digits[i++] = (char)('0' + (int)(value % 10));
        value /= 10;
    } while (value > 0 && i < (int)sizeof(digits));
    while (i > 0 && at + 1 < cap) {
        buffer[at++] = digits[--i];
    }
    return at;
}

int loop_line(int index, char *buffer, uint32_t cap)
{
    struct loop_device *loop;
    uint32_t at = 0;

    loop_settle();
    if (index < 0 || index >= LOOP_DEVICES || !loops[index].used || cap == 0) {
        return 0;
    }
    loop = &loops[index];
    at = put_text(buffer, at, cap, "loop");
    at = put_number(buffer, at, cap, (uint32_t)index);
    at = put_text(buffer, at, cap, " ");
    at = put_number(buffer, at, cap, (uint32_t)(loop->dev.sector_count * BLOCK_SECTOR_SIZE));
    at = put_text(buffer, at, cap, loop->writable ? " rw " : " ro ");
    at = put_text(buffer, at, cap, "writes ");
    at = put_number(buffer, at, cap, loop->writes);
    at = put_text(buffer, at, cap, " syncs ");
    at = put_number(buffer, at, cap, loop->syncs);
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
 * lock through open and publication.
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

/**
 * Sync the file just opened as `handle`, then check again, under the mount's
 * namespace lock, that its length is whole sectors and that it is not open
 * elsewhere, attached, or mapped shared. 0, or `-EINVAL` or `-EBUSY`, or what
 * the sync or the attribute read answered.
 *
 * Resolution and open run under the same lock, which the caller keeps
 * through publication on success, so no writer can relocate the node between
 * these steps. A shared mapping made while the sync slept is found here too,
 * since mmap does not take the lock: nothing between this check and the
 * publication sleeps.
 */
static int loop_prepare(struct vfs_ref *ref, void *handle, struct vfs_stat *st)
{
    int rc = vfs_fsync(ref->mnt, handle);

    if (rc == 0) {
        rc = vfs_getattr(ref->mnt, ref->node, st);
    }
    if (rc == 0 && (st->size == 0 || st->size % BLOCK_SECTOR_SIZE != 0)) {
        rc = -EINVAL;
    }
    if (rc == 0 && (fd_node_busy(ref->mnt, ref->node) || loop_backs(ref->mnt, ref->node) ||
                    filemap_disk_mapped(ref->mnt, st->ino))) {
        rc = -EBUSY;
    }
    return rc;
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
    loop->attaching = 1;
    loop->attached = 0;
    loop->in_flight = 0;
    loop->writes = 0;
    loop->syncs = 0;
    loop->release_due = 0;
    loop->autoclear = 0;
    loop->mnt = 0;
    loop->node = 0;
    loop->handle = 0;
    loop->dev.backing = 0;

    rc = loop_resolve(path, &ref);
    if (rc != 0) {
        loop_unclaim(loop);
        return rc;
    }
    rc = vfs_getattr(ref.mnt, ref.node, &st);
    if (rc != 0) {
        vfs_ns_unlock(ref.mnt);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return rc;
    }
    if ((st.mode & 0170000u) != 0100000u) {
        vfs_ns_unlock(ref.mnt);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EINVAL; /* only a regular file is an image */
    }
    if (st.size == 0 || st.size % BLOCK_SECTOR_SIZE != 0) {
        vfs_ns_unlock(ref.mnt);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EINVAL; /* whole sectors only */
    }
    if (loop_backs(ref.mnt, ref.node) || filemap_disk_mapped(ref.mnt, st.ino)) {
        vfs_ns_unlock(ref.mnt);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EBUSY;
    }
    if (block_depth(ref.mnt->dev) + 1 > BLOCK_MAX_DEPTH) {
        vfs_ns_unlock(ref.mnt);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -ELOOP; /* deeper than the four buffers a process may hold */
    }

    rc = vfs_open_node(ref.mnt, ref.node, writable ? 2 : 0, &handle);
    if (rc != 0) {
        vfs_ns_unlock(ref.mnt);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return rc;
    }
    rc = loop_prepare(&ref, handle, &st);
    if (rc != 0) {
        vfs_release(ref.mnt, handle);
        vfs_ns_unlock(ref.mnt);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return rc;
    }

    vfs_desc_open(ref.mnt);
    vfs_leave(ref.mnt);
    loop->index = index;
    loop->mnt = ref.mnt;
    loop->node = ref.node;
    loop->handle = handle;
    loop->writable = writable ? 1 : 0;
    loop->ino = (uint32_t)st.ino;
    k_strncpy(loop->path, path, sizeof(loop->path));
    loop->path[sizeof(loop->path) - 1] = '\0';
    loop->attaching = 0;
    loop->attached = 1;

    loop->dev.sector_size = BLOCK_SECTOR_SIZE;
    loop->dev.sector_count = st.size / BLOCK_SECTOR_SIZE;
    loop->dev.flags = BLOCK_CALLER_SYNC | (writable ? 0u : BLOCK_READ_ONLY);
    loop->dev.deadline_ticks = 0;
    loop->dev.read = loop_read;
    loop->dev.write = loop_write;
    loop->dev.submit = 0;
    loop->dev.flush = loop_flush;
    loop->dev.backing = ref.mnt->dev;
    loop->dev.drvdata = loop;

    vfs_ns_unlock(ref.mnt);

    klog(KLOG_INFO, "loop", "loop%d: %s, %d bytes, %s", index, loop->path, (int)st.size,
         loop->writable ? "read-write" : "read-only");
    return index;
}

/**
 * A slot claimed by an attachment still inside the steps that sleep is
 * refused: releasing it would free a claim about to publish itself, and the
 * next attach would then stand a second file under it.
 */
int loop_detach(int index)
{
    struct loop_device *loop;

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
        if (vfs_device_holds(&loop->dev)) {
            return -EBUSY;
        }
        if (loop->in_flight > 0) {
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
    if (loop->in_flight > 0) {
        return -EBUSY; /* a transfer is inside the file system the file is on */
    }
    if (bcache_sync(&loop->dev) != 0) {
        return -EIO;
    }
    if (bcache_invalidate(&loop->dev) != 0) {
        return -EBUSY;
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
    k_memcpy(info + INFO_INODE, &loop->ino, 4);
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
