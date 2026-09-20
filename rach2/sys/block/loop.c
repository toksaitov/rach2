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
#include "../fs/fd.h"
#include "../fs/vfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
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
    int in_flight; /* transfers inside the file system holding the file */
    /* What the device owes its writes and what it has paid: every write is a
     * write of the file and a sync of it, and /proc/loop reports both so a
     * test can ask whether the second half happened. */
    uint32_t writes;
    uint32_t syncs;
    int release_due;          /* the medium went during a transfer: the last one out releases */
    char path[LOOP_PATH_MAX]; /* what it was called when it was attached */
};

static struct loop_device loops[LOOP_DEVICES];

static struct loop_device *loop_of(block_device_t *device)
{
    return (struct loop_device *)device;
}

static void loop_release(struct loop_device *loop);
static void loop_unclaim(struct loop_device *loop);
static void loop_transfer_ended(struct loop_device *loop);
static void loop_settle(void);

/**
 * A transfer of whole sectors, through the file system holding the file.
 *
 * A short answer is a device error: the file's length fixed the sector count
 * at the attachment and nothing may change it while it is attached, so a read
 * or a write that does not move every byte means the file is not what it was.
 */
/**
 * Whether a transfer of `count` sectors at `lba` is one this device has.
 *
 * The block layer bounds every request before it calls a device, and this
 * asks the same of what arrives: a device that trusted its caller would read
 * or write past the end of the file it stands on, and a count that wrapped
 * the byte arithmetic would move nothing and report success.
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
    /* The cache's promise is that a written block has reached the device, and
     * this device is a file: the sync above is the half of the write that
     * makes that true. */
    return 0;
}

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

int loop_backs(struct vfs_mount *mnt, void *node)
{
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (loops[i].mnt != mnt) {
            continue;
        }
        /* By either name the file goes under: callers that have a node ask
         * with a node, and callers that have a description ask with the
         * handle the file system gave its open. */
        if (loops[i].node == node || loops[i].handle == node) {
            return 1;
        }
    }
    return 0;
}

/**
 * Let go of everything the attachment holds. The buffers go first: a device
 * whose file is released while the cache still has its blocks would write
 * them into a file nobody holds open.
 *
 * Never called while a transfer is inside the file system holding the file:
 * the caller checks, and a medium that goes during one leaves the release to
 * the transfer on its way out.
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

/**
 * The end of one transfer, and the release the medium's going left owing.
 */
static void loop_transfer_ended(struct loop_device *loop)
{
    loop->in_flight--;
}

/**
 * The release a medium's going left owing, done where it can be done.
 *
 * A device that goes is reported from wherever the driver noticed, which can
 * be the interrupt that found the stick missing, and letting go of what the
 * attachment holds takes a mount's lock and can sleep. So the going only
 * marks the device, and this runs at the next command through the control
 * node or the next reading of /proc/loop, both of which are syscalls.
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
        /* Nothing can reach the file again, so the device goes with it and
         * the mount above it can be taken down: an unmount of a device that
         * has gone writes nothing and only lets go of what it holds. What
         * this attachment holds goes back the same way, since the volume
         * holding the file cannot be unmounted while a description of it is
         * outstanding. */
        klog(KLOG_WARN, "loop", "loop%d: the device under its file has gone", i);
        loops[i].dev.flags |= BLOCK_GONE;
        loops[i].attached = 0;
        /* The slot stays taken until somebody detaches it: a mount may still
         * be standing on this device, and a fresh attachment here would put
         * another file under it. */
        loops[i].release_due = 1;
        bcache_device_gone(&loops[i].dev);
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
    /* Claimed before the steps below, every one of which can sleep: a second
     * attach that found this slot free would publish over this one, and what
     * the first had opened would have nobody left to release it. */
    loop->used = 1;
    loop->attaching = 1;
    loop->attached = 0;
    loop->in_flight = 0;
    loop->writes = 0;
    loop->syncs = 0;
    loop->release_due = 0;
    loop->mnt = 0;
    loop->node = 0;
    loop->handle = 0;
    /* The device this slot last stood on is not this one's: a medium going
     * while the steps below sleep must not match a claim that has published
     * nothing. */
    loop->dev.backing = 0;

    rc = vfs_resolve(0, path, 0, &ref, 0, 0);
    if (rc != 0) {
        loop_unclaim(loop);
        return rc;
    }
    rc = vfs_getattr(ref.mnt, ref.node, &st);
    if (rc != 0) {
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return rc;
    }
    if ((st.mode & 0170000u) != 0100000u) {
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EINVAL; /* only a regular file is an image */
    }
    if (st.size == 0 || st.size % BLOCK_SECTOR_SIZE != 0) {
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EINVAL; /* whole sectors, or the last one is a lie */
    }
    if (loop_backs(ref.mnt, ref.node)) {
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EBUSY;
    }
    if (block_depth(ref.mnt->dev) + 1 > BLOCK_MAX_DEPTH) {
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -ELOOP; /* deeper than the four buffers a process may hold */
    }

    rc = vfs_open_node(ref.mnt, ref.node, writable ? 2 : 0, &handle);
    if (rc != 0) {
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return rc;
    }
    /* A write or a truncation already inside the file system holding this
     * file would finish after the publication below and change an image the
     * mount above is reading. A description in the middle of one is what
     * says so, and this attach waits for another moment rather than taking
     * the file out from under it. */
    if (fd_node_busy(ref.mnt, ref.node)) {
        vfs_release(ref.mnt, handle);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EBUSY;
    }
    /* The open could sleep, and another attach could have taken this file in
     * the meantime; the publication below is what makes it this device's, and
     * nothing sleeps between here and it. */
    if (loop_backs(ref.mnt, ref.node)) {
        vfs_release(ref.mnt, handle);
        vfs_ref_release(&ref);
        loop_unclaim(loop);
        return -EBUSY;
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

    /* The resolver's hold on the node is the attachment's; released at the
     * detach through the description. */
    klog(KLOG_INFO, "loop", "loop%d: %s, %d bytes, %s", index, loop->path, (int)st.size,
         loop->writable ? "read-write" : "read-only");
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
    /* A slot claimed by an attachment still inside the steps that sleep:
     * releasing it here would free a claim that is about to publish itself,
     * and the next attach would then stand a second file under it. */
    if (loop->attaching) {
        return -EBUSY;
    }
    /* A device whose medium went has nothing left to write: the detach only
     * releases what the attachment still holds. */
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
        return -EBUSY; /* a transfer is inside the file system holding it */
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
 * A number and nothing else: no digits, a digit beyond the devices there are,
 * or anything but a space after it, and the command is not one.
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
    /* The one place a device number is read. No digits, a number beyond the
     * devices there are, or anything but the end of the word after it makes
     * the command invalid. */
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
