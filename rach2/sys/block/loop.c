/*
 * A block device over a regular file.
 *
 * Its read is a read of the file through the file system holding it, and its
 * write is a write followed by a sync of that file, so a block the buffer
 * cache says it has written is a block the file has taken. That is what lets
 * a file system live in a file: an image on a disk, on a stick, or on a root
 * that is nothing but memory.
 *
 * The device sleeps its caller and answers in the same call, which is the
 * third kind the block layer has (`BLOCK_CALLER_SYNC`): it must not take a
 * slot of the request pool, because the device under it takes one from the
 * same pool of sixteen while it waits. It names that device as the one it
 * stands on, and the buffer cache reads the chain to allow a caller holding
 * one of its blocks to take a block of the device below.
 *
 * A file is named by its mount and its node and not by its path, so a hard
 * link to an attached file is the same file and a second attachment of it is
 * refused. While a file is attached it is reached through the device and
 * nowhere else: a write, a truncation and an attribute that would change its
 * length are all refused, since the file system mounted on the device may
 * not have the image move under it.
 */

#include "loop.h"

#include "../fs/bcache.h"
#include "../fs/vfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../proc/process.h"

#define LOOP_PATH_MAX 64

struct loop_device {
    block_device_t dev; /* first: a device pointer is a loop pointer */
    int used;
    int index;
    struct vfs_mount *mnt;
    void *node;
    void *handle;
    int writable;
    char path[LOOP_PATH_MAX]; /* what it was called when it was attached */
};

static struct loop_device loops[LOOP_DEVICES];

static struct loop_device *loop_of(block_device_t *device)
{
    return (struct loop_device *)device;
}

/**
 * A transfer of whole sectors, through the file system holding the file.
 *
 * A short answer is a device error: the file's length fixed the sector count
 * at the attachment and nothing may change it while it is attached, so a read
 * or a write that does not move every byte means the file is not what it was.
 */
static int loop_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    struct loop_device *loop = loop_of(device);
    uint32_t bytes = count * BLOCK_SECTOR_SIZE;
    int32_t got;

    if (!loop->used) {
        return -EIO;
    }
    got = vfs_read(loop->mnt, loop->handle, buffer, bytes, lba * BLOCK_SECTOR_SIZE);
    if (got < 0 || (uint32_t)got != bytes) {
        klog(KLOG_WARN, "loop", "read: loop%d: sector %d: short by %d",
             loop->index, (int)lba, (int)(bytes - (got < 0 ? 0 : (uint32_t)got)));
        return -EIO;
    }
    return 0;
}

static int loop_write(block_device_t *device, uint64_t lba, uint32_t count, const void *buffer)
{
    struct loop_device *loop = loop_of(device);
    uint32_t bytes = count * BLOCK_SECTOR_SIZE;
    int32_t wrote;

    if (!loop->used) {
        return -EIO;
    }
    if (!loop->writable) {
        return -EROFS;
    }
    wrote = vfs_write(loop->mnt, loop->handle, buffer, bytes, lba * BLOCK_SECTOR_SIZE);
    if (wrote < 0 || (uint32_t)wrote != bytes) {
        klog(KLOG_WARN, "loop", "write: loop%d: sector %d: short by %d",
             loop->index, (int)lba, (int)(bytes - (wrote < 0 ? 0 : (uint32_t)wrote)));
        return -EIO;
    }
    /* The cache's promise is that a written block has reached the device, and
     * this device is a file: the sync is the half of the write that makes
     * that true. */
    if (vfs_fsync(loop->mnt, loop->handle) != 0) {
        return -EIO;
    }
    return 0;
}

static int loop_flush(block_device_t *device, struct block_request *request)
{
    struct loop_device *loop = loop_of(device);

    (void)request;
    if (!loop->used) {
        return -EIO;
    }
    return vfs_fsync(loop->mnt, loop->handle) == 0 ? 0 : -EIO;
}

block_device_t *loop_device_at(int index)
{
    if (index < 0 || index >= LOOP_DEVICES || !loops[index].used) {
        return 0;
    }
    return &loops[index].dev;
}

int loop_backs(struct vfs_mount *mnt, void *node)
{
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (loops[i].used && loops[i].mnt == mnt && loops[i].node == node) {
            return 1;
        }
    }
    return 0;
}

int loop_holds_mount(struct vfs_mount *mnt)
{
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (loops[i].used && loops[i].mnt == mnt) {
            return 1;
        }
    }
    return 0;
}

/**
 * Let go of everything the attachment holds. The buffers go first: a device
 * whose file is released while the cache still has its blocks would write
 * them into a file nobody holds open.
 */
static void loop_release(struct loop_device *loop)
{
    struct vfs_mount *mnt = loop->mnt;
    void *node = loop->node;
    void *handle = loop->handle;

    loop->used = 0;
    loop->mnt = 0;
    loop->node = 0;
    loop->handle = 0;
    loop->path[0] = '\0';
    bcache_invalidate(&loop->dev);
    if (mnt) {
        vfs_desc_close(mnt, node, handle);
    }
}

void loop_device_gone(block_device_t *device)
{
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (!loops[i].used || loops[i].dev.backing != device) {
            continue;
        }
        /* Nothing can reach the file again, so the device goes with it and
         * the mount above it can be taken down: an unmount of a device that
         * has gone writes nothing and only lets go of what it holds. */
        klog(KLOG_WARN, "loop", "loop%d: the device under its file has gone", i);
        loops[i].dev.flags |= BLOCK_GONE;
        bcache_device_gone(&loops[i].dev);
        loops[i].used = 0;
        loops[i].mnt = 0;
        loops[i].node = 0;
        loops[i].handle = 0;
        loops[i].path[0] = '\0';
    }
}

/** Append what fits of `text`, and answer where the next byte goes. */
static uint32_t put_text(char *buffer, uint32_t at, uint32_t cap, const char *text)
{
    while (*text && at + 1 < cap) {
        buffer[at++] = *text++;
    }
    return at;
}

/* Append a number the same way. A 32-bit one: this kernel has no 64-bit
 * division, and an image is addressed in 32 bits anyway. */
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

    if (index < 0 || index >= LOOP_DEVICES || !loops[index].used || cap == 0) {
        return 0;
    }
    loop = &loops[index];
    at = put_text(buffer, at, cap, "loop");
    at = put_number(buffer, at, cap, (uint32_t)index);
    at = put_text(buffer, at, cap, " ");
    at = put_number(buffer, at, cap,
                    (uint32_t)(loop->dev.sector_count * BLOCK_SECTOR_SIZE));
    at = put_text(buffer, at, cap, loop->writable ? " rw " : " ro ");
    at = put_text(buffer, at, cap, loop->path);
    at = put_text(buffer, at, cap, "\n");
    buffer[at] = '\0';
    return (int)at;
}

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
    loop = &loops[index];
    if (loop->used) {
        return -EBUSY;
    }

    rc = vfs_resolve(0, path, 0, &ref, 0, 0);
    if (rc != 0) {
        return rc;
    }
    rc = vfs_getattr(ref.mnt, ref.node, &st);
    if (rc != 0) {
        vfs_ref_release(&ref);
        return rc;
    }
    if ((st.mode & 0170000u) != 0100000u) {
        vfs_ref_release(&ref);
        return -EINVAL; /* only a regular file is an image */
    }
    if (st.size == 0 || st.size % BLOCK_SECTOR_SIZE != 0) {
        vfs_ref_release(&ref);
        return -EINVAL; /* whole sectors, or the last one is a lie */
    }
    if (loop_backs(ref.mnt, ref.node)) {
        vfs_ref_release(&ref);
        return -EBUSY; /* one file, one device, whatever it is called */
    }
    if (block_depth(ref.mnt->dev) + 1 > BLOCK_MAX_DEPTH) {
        vfs_ref_release(&ref);
        return -ELOOP; /* deeper than the four buffers a process may hold */
    }

    rc = vfs_open_node(ref.mnt, ref.node, writable ? 2 : 0, &handle);
    if (rc != 0) {
        vfs_ref_release(&ref);
        return rc;
    }

    /* The description keeps the mount, so unmounting the volume holding the
     * file answers EBUSY while the device is attached. The resolution's own
     * count of an operation in progress is given back here, the way an open
     * does it: what the attachment keeps is the hold and the description,
     * not a syscall that never ends. */
    vfs_desc_open(ref.mnt);
    vfs_leave(ref.mnt);
    loop->index = index;
    loop->mnt = ref.mnt;
    loop->node = ref.node;
    loop->handle = handle;
    loop->writable = writable ? 1 : 0;
    k_strncpy(loop->path, path, sizeof(loop->path));
    loop->path[sizeof(loop->path) - 1] = '\0';

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
    loop->used = 1;

    /* The resolver's hold on the node is the attachment's; released at the
     * detach through the description. */
    klog(KLOG_INFO, "loop", "loop%d: %s, %d bytes, %s", index, loop->path,
         (int)st.size, loop->writable ? "read-write" : "read-only");
    return index;
}

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
    if (vfs_device_holds(&loop->dev)) {
        return -EBUSY; /* something is mounted on it */
    }
    if (bcache_device_holds(&loop->dev)) {
        return -EBUSY; /* a buffer of it is held */
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
 * The commands /dev/loop-control takes, one per write: `attach N PATH`,
 * `attach - PATH` for the first free device, and `detach N`.
 *
 * A line and not a structure, because the only caller is a program the
 * machine builds itself and a line is what a shell can send.
 */
int loop_control_write(const char *buffer, uint32_t count)
{
    char line[LOOP_PATH_MAX + 16];
    char *word;
    int index = -1;
    uint32_t i;

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

        while (*arg == ' ') {
            arg++;
        }
        if (*arg == '\0') {
            return -EINVAL;
        }
        if (*arg != '-') {
            index = 0;
            while (*arg >= '0' && *arg <= '9') {
                index = index * 10 + (*arg - '0');
                arg++;
            }
        } else {
            arg++;
        }
        while (*arg == ' ') {
            arg++;
        }
        path = arg;
        if (*path != '/') {
            return -EINVAL; /* an absolute path, since the device has no cwd */
        }
        return loop_attach(index, path, 1);
    }
    if (word_is(word, "detach ")) {
        char *arg = word + 7;

        while (*arg == ' ') {
            arg++;
        }
        index = 0;
        while (*arg >= '0' && *arg <= '9') {
            index = index * 10 + (*arg - '0');
            arg++;
        }
        return loop_detach(index);
    }
    return -EINVAL;
}
