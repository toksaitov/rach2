/*
 * KRFFS as a mounted volume.
 *
 * The format is in krffs.c, which addresses nodes by their offset from the
 * first byte of the region and reaches the medium through a backend. This
 * file is the other backend, over a block device through the buffer cache,
 * and the VFS type that `mount(2)` places. The boot root is a mapped image
 * with a pointer interface, in krffs.c, and never comes through here.
 */

#include "krffsmount.h"

#include "../klog.h"
#include "../proc/process.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../block/block.h"
#include "bcache.h"
#include "krffs.h"
#include "vfs.h"

/* The files a volume has open, one record per open node, allocated at the
 * first open and freed at the last close. A name removed while a file is
 * open takes the name off the node and leaves the node, the way the root
 * does, so whoever holds it keeps reading it; the last close frees it. An
 * O_PATH description, which opens nothing, keeps its node by a pin counted
 * in the same record, and unlink and rename treat a pinned node as an open
 * one. */
struct krffs_open {
    struct krffs_open *next;
    uint64_t offset; /* where the node is now, which a move updates */
    int refs;        /* opens not yet released */
    int nameless;    /* unlinked while open: freed at the last close */
    int pins;        /* O_PATH descriptions that keep the node, opening nothing */
};

/* A mounted volume, allocated at mount and freed at unmount. */
struct krffs_volume {
    block_device_t *dev;
    struct vfs_mount *mnt;
    krffs_mount_t fs;         /* the region, which is the whole device */
    int writable;             /* set at mount; VFS_MS_RDONLY in mnt->flags stops writes later */
    struct krffs_open *opens; /* the open records, one per node */
    /* The open file a write or truncate under the mount's lock is for, whose
     * record tags the blocks the backend dirties meanwhile; NULL otherwise. */
    struct krffs_open *writer;
};

/** The volume of the mount `mnt`. */
static struct krffs_volume *vol_of(struct vfs_mount *mnt)
{
    return (struct krffs_volume *)mnt->priv;
}

/**
 * Encode a node offset for the VFS.
 *
 * Adding one keeps the root's zero offset distinct from a NULL node.
 */
static void *handle_of(uint64_t offset)
{
    return (void *)(uintptr_t)(offset + 1);
}

/** The node offset `handle_of` encoded in `handle`; a NULL handle, which the
 * VFS passes for the mount root, is offset 0. */
static uint64_t offset_of(struct vfs_mount *mnt, void *handle)
{
    (void)mnt;
    if (!handle) {
        return 0;
    }
    return (uint64_t)(uintptr_t)handle - 1;
}

/*
 * The block backend: the region's bytes, through the buffer cache.
 *
 * The offsets are from the start of the volume, which is the start of the
 * device the mount was given, so a partition window needs no arithmetic here.
 * A store across a block edge holds two buffers in increasing block order.
 */

/**
 * Copy `size` bytes at `offset` of the volume `ctx` into `buffer`, a cache
 * block at a time. 0, or -1 for a block the cache cannot give or bytes past
 * the end of the device. May sleep in the cache.
 */
static int volume_read(void *ctx, uint64_t offset, void *buffer, uint32_t size)
{
    struct krffs_volume *vol = ctx;
    uint8_t *out = buffer;

    while (size > 0) {
        uint64_t block = offset / BCACHE_BLOCK_BYTES;
        uint32_t within = (uint32_t)(offset % BCACHE_BLOCK_BYTES);
        uint32_t take = BCACHE_BLOCK_BYTES - within;
        struct bbuf *b;

        if (take > size) {
            take = size;
        }
        b = bcache_get(vol->dev, block);
        if (!b) {
            return -1;
        }
        if (within + take > b->bytes ||
            (!vol->fs.halted && (vol->dev->flags & BLOCK_WRITES_HALTED))) {
            bcache_release(b);
            return -1;
        }
        k_memcpy(out, b->data + within, take);
        bcache_release(b);
        out += take;
        offset += take;
        size -= take;
    }
    return 0;
}

/** The buffer cache's owner of the blocks an open file's operations dirty:
 * its record, which follows the node through every move. */
static uint64_t owner_of(const struct krffs_open *o)
{
    return (uint64_t)(uintptr_t)o;
}

/** Copy into a held, checked range and tag its block for file writeback. */
static void volume_store(struct krffs_volume *vol, struct bbuf *b, uint32_t at, const uint8_t *src,
                         uint32_t n)
{
    k_memcpy(b->data + at, src, n);
    if (vol->writer) {
        bcache_dirty_owned(b, owner_of(vol->writer));
    } else {
        bcache_dirty(b);
    }
}

/**
 * Store a field or one unit of file data in the volume's buffer cache.
 * The core cuts payload writes at block boundaries. A header, field or
 * internal copy fits in one block's worth of bytes but may cross an edge.
 * Take both blocks in ascending order and check the short final block
 * before changing either. Return 0 or -1 with no part of the store made.
 */
static int volume_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    struct krffs_volume *vol = ctx;
    uint64_t block = offset / BCACHE_BLOCK_BYTES;
    uint32_t at = (uint32_t)(offset % BCACHE_BLOCK_BYTES);
    uint32_t first = BCACHE_BLOCK_BYTES - at;
    struct bbuf *b, *next = NULL;
    int rc = -1;

    if (!vol->writable || vol->fs.halted || size > BCACHE_BLOCK_BYTES) {
        return -1;
    }
    if (!size) {
        return 0;
    }
    if (first > size) {
        first = size;
    }
    b = bcache_get(vol->dev, block);
    if (!b) {
        return -1;
    }
    if (first < size) {
        next = bcache_get(vol->dev, block + 1);
    }
    if (!(vol->dev->flags & BLOCK_WRITES_HALTED) && at + first <= b->bytes &&
        (first == size || (next && size - first <= next->bytes))) {
        volume_store(vol, b, at, buffer, first);
        if (next) {
            volume_store(vol, next, 0, (const uint8_t *)buffer + first, size - first);
        }
        rc = 0;
    }
    if (next) {
        bcache_release(next);
    }
    bcache_release(b);
    return rc;
}

/**
 * The format's sync point: the cache's dirty buffers to the device, and then
 * the device's own cache to the medium; 0, or -1 if either fails. A stick
 * answers a write as soon as it has taken it, so a volume whole on the wire
 * is not yet whole on the medium, and the order this file system's protocol
 * rests on would be an order the medium never saw.
 */
static int volume_sync(void *ctx)
{
    struct krffs_volume *vol = ctx;

    if (bcache_sync_unflushed(vol->dev) != 0) {
        return -1;
    }
    return bcache_flush(vol->dev) == 0 ? 0 : -1;
}

/**
 * The core halted the volume (`krffs_withdraw`): every mount that shows it
 * turns read-only (`vfs_mount_halted`), so that an open for writing, a
 * creation or a link is refused with -EROFS before it reaches the core.
 * The cache writes none of the volume's blocks after this
 * (`bcache_halt_writes`), for a sync, an eviction or the flusher, so the
 * medium keeps what the last sync point wrote, the dirty flag the mount set
 * among it, and the unmount drops what is left dirty.
 */
static void kv_halted(krffs_mount_t *fs)
{
    struct krffs_volume *vol = fs->ctx;

    bcache_halt_writes(vol->dev);
    vfs_mount_halted(vol->mnt);
}

/** Reap a last holder's node under the mount lock. A failed writeback
 * during the reap blocks later requests, including other cache victims. */
static int kv_reap(struct krffs_volume *vol, uint64_t offset)
{
    int rc;

    bcache_recovering(vol->dev, 1);
    rc = krffs_reap_at(&vol->fs, offset);
    bcache_recovering(vol->dev, 0);
    return rc;
}

/**
 * Whether the mount takes no change: mounted read-only, made read-only by a
 * failed write, or halted by the core (`krffs_withdraw`), which refuses every
 * write until the next mount, as Linux's ext4 turns read-only after an error.
 */
static int refuses_writes(struct krffs_volume *vol, struct vfs_mount *mnt)
{
    return !vol->writable || (mnt->flags & VFS_MS_RDONLY) || vol->fs.halted;
}

/**
 * The mount's lock and the writable check for write, truncate and setattr;
 * the namespace operations arrive locked. Returns 0 with the lock held, or
 * -EROFS with it released. Decided under the lock, so a writer queued behind
 * one whose write failed finds the mount as it now is and not as it was.
 */
static int begin_write(struct vfs_mount *mnt)
{
    struct krffs_volume *vol;

    vfs_ns_lock(mnt);
    vol = vol_of(mnt);
    if (refuses_writes(vol, mnt)) {
        vfs_ns_unlock(mnt);
        return -EROFS;
    }
    return 0;
}

/**
 * The path of `dir` within the volume with `name` joined to it.
 *
 * The core's namespace operations take a path, which is what the ring is
 * searched by; the mount table hands out a directory and a name, so they are
 * put back together here, as the root's adapter does. 0, -EIO when the
 * directory's path cannot be read, or -ENAMETOOLONG when the path does not
 * fit in `cap` bytes or the directory's path cannot be made.
 */
static int join(struct vfs_mount *mnt, void *dir, const char *name, char *path, uint32_t cap)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint32_t dlen;
    uint32_t nlen;
    int rc = krffs_node_path_at(&vol->fs, offset_of(mnt, dir), path, cap);

    if (rc != 0) {
        return rc == -EIO ? -EIO : -ENAMETOOLONG;
    }
    dlen = (uint32_t)k_strlen(path);
    nlen = (uint32_t)k_strlen(name);
    if (dlen == 0 || path[dlen - 1] != '/') {
        if (dlen + 1 >= cap) {
            return -ENAMETOOLONG;
        }
        path[dlen++] = '/';
    }
    if (dlen + nlen + 1 > cap) {
        return -ENAMETOOLONG;
    }
    k_memcpy(path + dlen, name, nlen + 1);
    return 0;
}

/* A write inside one block is whole or not at all: the block is read before
 * a byte is copied (`volume_write`). */
static const krffs_backend_t volume_backend = {volume_read, volume_write, volume_sync,
                                               BCACHE_BLOCK_BYTES};

/* The operations. */

/**
 * The node `name` names in the directory `dir`: `dir` itself for `.`, its
 * parent for `..`, the root's parent being the root, and otherwise its
 * child, or -ENOENT, or -EIO for a read that failed before it was found.
 * A handle is no resource, so nothing is held.
 */
static int kv_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint64_t at = offset_of(mnt, dir);
    uint64_t found;
    int rc;

    if (name[0] == '.' && name[1] == '\0') {
        *node = handle_of(at);
        return 0;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        uint64_t parent = krffs_parent_offset_err(&vol->fs, at, &rc);

        if (rc != 0) {
            return rc;
        }
        *node = handle_of(parent == KRFFS_NO_NODE ? 0 : parent);
        return 0;
    }
    found = krffs_child_at_err(&vol->fs, at, name, &rc);
    if (found == KRFFS_NO_NODE) {
        return rc;
    }
    *node = handle_of(found);
    return 0;
}

/**
 * The attributes of `node` from its header, or -EIO when it cannot be read.
 *
 * The root directory's mode is S_IFDIR and the stored bits
 * KRFFS_ROOT_MODE_BITS selects, which leave out the dirty flag that shares
 * the field. A directory has two links and any other node one. The inode
 * number is the offset plus one, unique at any moment, so a node that moves
 * gets a new number.
 */
static int kv_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint64_t at = offset_of(mnt, node);
    krffs_node_t head;

    if (krffs_head(&vol->fs, at, &head) != 0) {
        return -EIO;
    }
    k_memset(st, 0, sizeof(*st));
    if (at == 0) {
        st->mode = KRFFS_S_IFDIR | (head.mode & KRFFS_ROOT_MODE_BITS);
        st->nlink = 2;
    } else {
        st->mode = krffs_mode_type(head.mode) | (head.mode & 07777u);
        st->nlink = KRFFS_IS_DIR(head.mode) ? 2 : 1;
    }
    st->uid = head.uid;
    st->gid = head.gid;
    st->ino = at + 1;
    st->size = head.data_size;
    st->atime_sec = head.access_time.seconds;
    st->atime_nsec = head.access_time.nanoseconds;
    st->mtime_sec = head.modification_time.seconds;
    st->mtime_nsec = head.modification_time.nanoseconds;
    st->ctime_sec = head.attribute_change_time.seconds;
    st->ctime_nsec = head.attribute_change_time.nanoseconds;
    return 0;
}

/** The record of an open file at `at`, or NULL. */
static struct krffs_open *open_at(struct krffs_volume *vol, uint64_t at)
{
    struct krffs_open *o;

    for (o = vol->opens; o; o = o->next) {
        if (o->offset == at) {
            return o;
        }
    }
    return 0;
}

/** Follow an open record to where the node moved. */
static void open_moved(struct krffs_volume *vol, uint64_t from, uint64_t to)
{
    struct krffs_open *o = open_at(vol, from);

    if (o) {
        o->offset = to;
    }
}

/**
 * Resolve an open file's current offset under the mount lock.
 *
 * The handle identifies the same file across every relocation while
 * the caller waits. Its open reference keeps the record alive.
 */
static uint64_t follow_moves(const struct krffs_open *handle)
{
    return handle->offset;
}

/**
 * Return a shared open record that remains valid across relocation.
 *
 * The record tracks the node's current offset and remains allocated until
 * its last open reference is released. The caller holds the namespace
 * lock from lookup through registration, so `node` cannot become stale.
 * 0, or -ENOMEM when a new record cannot be allocated.
 */
static int kv_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint64_t at = offset_of(mnt, node);
    struct krffs_open *o = open_at(vol, at);

    (void)flags;
    if (!o) {
        o = kzalloc(sizeof(*o));
        if (!o) {
            return -ENOMEM;
        }
        o->offset = at;
        o->next = vol->opens;
        vol->opens = o;
    }
    o->refs++;
    *handle = o;
    return 0;
}

/** The `handle_node` of `vfs_ops`: the node the open record `handle` is for,
 * encoded at the offset where the node is now, which a move updates. */
static void *kv_handle_node(void *handle)
{
    return handle_of(((struct krffs_open *)handle)->offset);
}

/** The `phase` of `vfs_ops`: where in a sector of the medium the first
 * data byte of the file open as `handle` lies, from the volume's own phase
 * and the node's offset, which a move updates. */
static uint32_t kv_phase_of(struct vfs_mount *mnt, void *node, void *handle)
{
    uint64_t at = follow_moves((const struct krffs_open *)handle);

    (void)node;
    return krffs_file_phase(&vol_of(mnt)->fs, at);
}

/**
 * Free a record that no open and no pin keeps, and reap its file when it
 * was unlinked meanwhile and the mount is still writable; on a read-only
 * mount the node stays, without a name, for a later repair
 * (`krffs_repair_at` or `fsck.krffs`).
 *
 * Remove the record before reaping, which may sleep under the mount lock.
 */
static void open_drop(struct vfs_mount *mnt, struct krffs_open *o)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint64_t at = o->offset;
    int reap = o->nameless;
    struct krffs_open **link;

    for (link = &vol->opens; *link != o; link = &(*link)->next) {
    }
    *link = o->next;
    kfree(o);
    if (reap && vol->writable && !(mnt->flags & VFS_MS_RDONLY)) {
        vfs_ns_lock(mnt);
        (void)kv_reap(vol, at);
        vfs_ns_unlock(mnt);
    }
}

/**
 * Release an open record and reap an unlinked file at its last close, when
 * no pin keeps the node besides.
 */
static void kv_release(struct vfs_mount *mnt, void *handle)
{
    struct krffs_open *o = handle;

    if (!o) {
        return;
    }
    if (--o->refs > 0 || o->pins > 0) {
        return;
    }
    open_drop(mnt, o);
}

/**
 * Count a pin of `node` for an O_PATH description, which opens nothing, in
 * the node's open record, made if it has none.
 *
 * Unlink and rename leave a pinned node, nameless, as they leave an open
 * one, and the record follows the node where a write moves it. The caller
 * holds the mount's lock from the lookup, so the node is the one it found.
 * `*token` is the record, for `kv_unpin`.
 */
static int kv_pin(struct vfs_mount *mnt, void *node, void **token)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint64_t at = offset_of(mnt, node);
    struct krffs_open *o = open_at(vol, at);

    if (!o) {
        o = kzalloc(sizeof(*o));
        if (!o) {
            return -ENOMEM;
        }
        o->offset = at;
        o->next = vol->opens;
        vol->opens = o;
    }
    o->pins++;
    *token = o;
    return 0;
}

/**
 * Drop a pin `kv_pin` counted. The last pin of a node that no open keeps
 * frees the record, and the node too when its name was removed.
 */
static void kv_unpin(struct vfs_mount *mnt, void *token)
{
    struct krffs_open *o = token;

    if (--o->pins > 0 || o->refs > 0) {
        return;
    }
    open_drop(mnt, o);
}

/**
 * Read an open file under the mount lock.
 *
 * The open record retains its identity across relocation and reuse of the
 * old offset, including while the caller waits for the lock. Returns the
 * bytes read, 0 at an offset past 4 GiB or the end of the file, or -EIO.
 */
static int32_t kv_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                       uint64_t offset)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint64_t at;
    int got;

    if (offset > 0xFFFFFFFFull) {
        return 0;
    }
    vfs_ns_lock(mnt);
    at = follow_moves(handle);
    got = krffs_read_at(&vol->fs, at, buf, count, (uint32_t)offset);
    vfs_ns_unlock(mnt);
    return got < 0 ? -EIO : (int32_t)got;
}

/* The cursor of a listing that has passed its last entry. */
#define KRFFS_DIR_END 0xFFFFFFFFu

/**
 * Read one directory entry under the mount lock through its open handle.
 *
 * The cursor is the next node's offset. Zero starts at the root, which
 * is not an entry; returning to it ends the walk. The entry's inode number
 * is its offset plus one, as `kv_getattr` gives it. Returns 1 with `ent`
 * filled, 0 at the end, -EIO for a header that cannot be read, or -ENOTDIR
 * for a handle that is not a directory.
 */
static int kv_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint64_t dir;
    uint64_t dir_id = KRFFS_ROOT_ID;
    uint64_t scan = *cursor;
    uint64_t steps = 0;
    krffs_node_t head;

    if (*cursor == KRFFS_DIR_END) {
        return 0;
    }
    vfs_ns_lock(mnt);
    dir = follow_moves(handle);
    if (dir != 0) {
        if (krffs_head(&vol->fs, dir, &head) != 0) {
            vfs_ns_unlock(mnt);
            return -EIO;
        }
        if (!KRFFS_IS_DIR(head.mode)) {
            vfs_ns_unlock(mnt);
            return -ENOTDIR;
        }
        dir_id = head.id;
    }

    do {
        if (krffs_head(&vol->fs, scan, &head) != 0) {
            vfs_ns_unlock(mnt);
            return -EIO;
        }
        if (head.type == KRFFS_RESERVED_NODE && head.parent_id == dir_id && head.name[0] != '\0' &&
            !krffs_hidden(&vol->fs, scan)) {
            uint32_t i;
            uint64_t next = krffs_next_offset(&vol->fs, scan, &head);

            for (i = 0; i + 1 < sizeof(ent->name) && head.name[i] != '\0'; i++) {
                ent->name[i] = (char)head.name[i];
            }
            ent->name[i] = '\0';
            ent->ino = scan + 1;
            ent->is_dir = KRFFS_IS_DIR(head.mode) ? 1 : 0;
            ent->type = (int)VFS_DT_OF(krffs_mode_type(head.mode));
            *cursor = next == 0 ? KRFFS_DIR_END : (uint32_t)next;
            vfs_ns_unlock(mnt);
            return 1;
        }
        scan = krffs_next_offset(&vol->fs, scan, &head);
    } while (scan != 0 && ++steps < krffs_max_steps(&vol->fs));

    *cursor = KRFFS_DIR_END;
    vfs_ns_unlock(mnt);
    return 0;
}

/**
 * Write an open file under the mount lock.
 *
 * Resolve the handle after taking the mount lock. If the node moves,
 * repoint its references from the resolved offset to the new offset,
 * including when the operation fails after relocation. Returns the bytes
 * written; -EROFS on a read-only mount; -EFBIG for a write that would reach
 * past 4 GiB; -ENOSPC when the file could not grow; or -EIO.
 */
static int32_t kv_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                        uint64_t offset)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint64_t at;
    uint64_t was;
    int written;
    int rc = begin_write(mnt);

    if (rc != 0) {
        return rc;
    }
    at = follow_moves(handle);
    was = at;
    if (offset > 0xFFFFFFFFull || offset + count > 0xFFFFFFFFull) {
        vfs_ns_unlock(mnt);
        return -EFBIG;
    }
    vol->writer = handle;
    written = krffs_write_at(&vol->fs, &at, buf, count, (uint32_t)offset);
    vol->writer = NULL;
    if (at != KRFFS_NO_NODE && at != was) {
        open_moved(vol, was, at);
        vfs_repoint(mnt, handle_of(was), handle_of(at));
    }
    vfs_ns_unlock(mnt);
    if (written == KRFFS_ENOSPACE) {
        return -ENOSPC;
    }
    if (written < 0) {
        return -EIO;
    }
    return (int32_t)written;
}

/**
 * Truncate an open file under the mount lock.
 *
 * Resolve the handle after taking the mount lock. If the node moves,
 * repoint its references from the resolved offset to the new offset,
 * including when the operation fails after relocation. Returns 0; -EROFS on
 * a read-only mount; -EFBIG for a size of 4 GiB or more; -ENOSPC when the
 * file could not grow; or -EIO.
 */
static int kv_truncate(struct vfs_mount *mnt, void *handle, uint64_t size, int stamp_times)
{
    struct krffs_volume *vol = vol_of(mnt);
    int rc = begin_write(mnt);

    if (rc != 0) {
        return rc;
    }
    if (size > 0xFFFFFFFFull) {
        vfs_ns_unlock(mnt);
        return -EFBIG;
    }
    {
        uint64_t was = follow_moves(handle);
        uint64_t now = was;

        vol->writer = handle;
        rc = krffs_resize_node_at(&vol->fs, now, (uint32_t)size, &now, stamp_times);
        vol->writer = NULL;
        if (now != KRFFS_NO_NODE && now != was) {
            open_moved(vol, was, now);
            vfs_repoint(mnt, handle_of(was), handle_of(now));
        }
    }
    vfs_ns_unlock(mnt);
    if (rc == KRFFS_ENOSPACE) {
        return -ENOSPC;
    }
    return rc == 0 ? 0 : -EIO;
}

/** Write the cache blocks that hold bytes `at` to `at + len - 1` of the
 * volume, whatever their tags; 0 or the first write's -errno. */
static int sync_bytes(struct krffs_volume *vol, uint64_t at, uint64_t len)
{
    uint64_t blk;
    int rc = 0;

    for (blk = at / BCACHE_BLOCK_BYTES; rc == 0 && blk <= (at + len - 1) / BCACHE_BLOCK_BYTES;
         blk++) {
        rc = bcache_sync_block(vol->dev, blk);
    }
    return rc;
}

/**
 * Write the backward link a resize of the node at `at` may have left dirty:
 * that of the node after the free node that follows it, which a shrink and a
 * grow with a surplus fix after their last sync point. Nothing when the next
 * node is not free, since every change of the ring between them ends at a
 * sync point. 0, -EIO for a header that cannot be read, or the write's
 * -errno.
 */
static int sync_link_after(struct krffs_volume *vol, uint64_t at)
{
    krffs_node_t head;
    uint64_t next;

    if (krffs_head(&vol->fs, at, &head) != 0) {
        return -EIO;
    }
    next = krffs_next_offset(&vol->fs, at, &head);
    if (next == 0) {
        return 0;
    }
    if (krffs_head(&vol->fs, next, &head) != 0) {
        return -EIO;
    }
    if (head.type != KRFFS_FREE_NODE) {
        return 0;
    }
    return sync_bytes(vol, krffs_next_offset(&vol->fs, next, &head) + KRFFS_FIELD_PREVIOUS,
                      sizeof(head.previous_node_size));
}

/**
 * Write what an open file left dirty, under the mount lock: the blocks its
 * writes and truncations dirtied, then the blocks of its header, then the
 * backward link its last resize fixed. The caller, `vfs_fsync`, flushes the
 * device after.
 *
 * Every write of the core that changes the ring or stores data ends at a
 * sync point, which writes the whole device, so what an operation leaves
 * dirty is header fields: the file's length, size and times, and the
 * backward link of the node after a shrunk one. The header and the link are
 * written by their offsets, whatever tags their blocks have: a change of
 * mode, owner or times dirties the header outside a write, a neighbour's
 * write or truncate retags a block, and a file closed and opened again has
 * a new record and so a new tag. A NULL handle, a sync of the whole volume,
 * and a mount a failed write made read-only write every dirty buffer of the
 * device, since a sync point that failed leaves blocks of any file dirty,
 * the failed one too.
 *
 * Returns 0 or the writeback's -errno, with the buffers that failed still
 * dirty.
 */
static int kv_fsync(struct vfs_mount *mnt, void *handle)
{
    struct krffs_volume *vol = vol_of(mnt);
    struct krffs_open *o = handle;
    uint64_t at;
    int rc;

    vfs_ns_lock(mnt);
    if (o && vol->fs.halted) {
        vfs_ns_unlock(mnt);
        return -EIO;
    }
    if (!o || !vol->writable || (mnt->flags & VFS_MS_RDONLY)) {
        rc = bcache_sync(vol->dev);
        vfs_ns_unlock(mnt);
        return rc;
    }
    at = follow_moves(o);
    rc = bcache_sync_owned(vol->dev, owner_of(o));
    if (rc == 0) {
        rc = sync_bytes(vol, at, sizeof(krffs_node_t));
    }
    if (rc == 0) {
        rc = sync_link_after(vol, at);
    }
    vfs_ns_unlock(mnt);
    return rc;
}

/**
 * The errno of a creation that failed in the core: -ENOSPC for a volume with
 * no room; -EINVAL for a directory whose identifier another directory has
 * (`KRFFS_ETAKEN`), the answer for a name the file system cannot hold; and
 * -EIO for a read, a write or a sync of the medium that failed, which a
 * device removed during the creation causes. A path the core could not use
 * is -EIO too: the directory the name goes in is held, so only a failed
 * read loses it.
 */
static int made_errno(int rc)
{
    if (rc == KRFFS_ETAKEN) {
        return -EINVAL;
    }
    return rc == KRFFS_ENOSPACE ? -ENOSPC : -EIO;
}

/** The owner a node is made with: the caller's file-system user id, which
 * the core writes before the node's name, and root's when there is no
 * caller. */
static uint32_t maker_uid(void)
{
    return current_proc ? proc_fsuid(current_proc) : 0;
}

/**
 * The group a node made in `dir` has, in `*gid`, and the set-group-ID bit a
 * directory made there takes, in `*bit`, as Linux's `inode_init_owner` gives
 * them: a set-group-ID directory's group and its bit, or else the caller's
 * file-system group id, root's when there is no caller, and no bit. 0, or
 * -EIO when the directory's header cannot be read.
 */
static int maker_gid(struct vfs_mount *mnt, void *dir, uint32_t *gid, uint32_t *bit)
{
    uint64_t at = offset_of(mnt, dir);
    krffs_node_t head;

    *gid = current_proc ? proc_fsgid(current_proc) : 0;
    *bit = 0;
    if (krffs_head(&vol_of(mnt)->fs, at, &head) != 0) {
        return -EIO;
    }
    if (head.mode & 02000u) {
        *gid = head.gid;
        *bit = 02000u;
    }
    return 0;
}

/**
 * Create `name` in `dir`, with the caller's `mode`, already masked by its
 * umask, and the caller's identity, both on the medium before the name; its
 * group is that of `maker_gid`.
 *
 * A name that is already there answers -EEXIST. The core would answer
 * success, which would make this a creation of somebody else's file: its
 * mode, its owner and the caller's O_EXCL all belong to the open that finds
 * it. Otherwise -EROFS on a read-only mount, -ENAMETOOLONG for a path that
 * does not fit, `made_errno`'s answer for a creation that failed in the
 * core, and -EIO when the new node cannot be found.
 */
static int kv_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node)
{
    struct krffs_volume *vol = vol_of(mnt);
    char path[256];
    uint64_t made;
    uint32_t gid, bit;
    int rc;

    if (refuses_writes(vol, mnt)) {
        return -EROFS;
    }
    rc = join(mnt, dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    if (krffs_child_at_err(&vol->fs, offset_of(mnt, dir), name, &rc) != KRFFS_NO_NODE) {
        return -EEXIST;
    }
    if (rc == -EIO) {
        return rc;
    }
    rc = maker_gid(mnt, dir, &gid, &bit);
    if (rc != 0) {
        return rc;
    }
    rc = krffs_create_node_at(&vol->fs, path, (uint16_t)(mode & 07777u), maker_uid(), gid);
    if (rc != 0) {
        return made_errno(rc);
    }
    made = krffs_child_at(&vol->fs, offset_of(mnt, dir), name);
    if (made == KRFFS_NO_NODE) {
        return -EIO;
    }
    *node = handle_of(made);
    return 0;
}

/**
 * A FIFO or a socket `name` in `dir`, made as `kv_create` makes a file, with
 * its type and permission bits and its owner on the medium before its name.
 * A device node has a number the format has no field for, and answers
 * -EPERM. Otherwise the errors are `kv_create`'s.
 */
static int kv_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                    uint32_t rdev, void **node)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint32_t type = mode & KRFFS_S_IFMT;
    char path[256];
    uint64_t made;
    uint32_t gid, bit;
    int rc;

    (void)rdev;
    if (refuses_writes(vol, mnt)) {
        return -EROFS;
    }
    if (type != KRFFS_S_IFIFO && type != KRFFS_S_IFSOCK) {
        return -EPERM;
    }
    rc = join(mnt, dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    if (krffs_child_at_err(&vol->fs, offset_of(mnt, dir), name, &rc) != KRFFS_NO_NODE) {
        return -EEXIST;
    }
    if (rc == -EIO) {
        return rc;
    }
    rc = maker_gid(mnt, dir, &gid, &bit);
    if (rc != 0) {
        return rc;
    }
    rc = krffs_create_node_at(&vol->fs, path, (uint16_t)(type | (mode & 07777u)), maker_uid(), gid);
    if (rc != 0) {
        return made_errno(rc);
    }
    made = krffs_child_at(&vol->fs, offset_of(mnt, dir), name);
    if (made == KRFFS_NO_NODE) {
        return -EIO;
    }
    *node = handle_of(made);
    return 0;
}

/**
 * Remove `name` from `dir`.
 *
 * A name taken off a file somebody has open leaves the node, which keeps
 * that reader's file alive, and the last close frees it. The open record is
 * looked up again after the writes, which sleep: if a last close freed it
 * meanwhile, the node goes here. 0, -EROFS on a read-only mount,
 * -ENAMETOOLONG, -ENOENT for a missing name, -EISDIR for a directory, as the
 * boot root answers, or -EIO for a name the core does not remove.
 */
static int kv_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct krffs_volume *vol = vol_of(mnt);
    char path[256];
    int rc;

    if (refuses_writes(vol, mnt)) {
        return -EROFS;
    }
    rc = join(mnt, dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    {
        uint64_t at = krffs_child_at_err(&vol->fs, offset_of(mnt, dir), name, &rc);
        krffs_node_t head;

        if (at == KRFFS_NO_NODE) {
            return rc;
        }
        if (krffs_head(&vol->fs, at, &head) != 0) {
            return -EIO;
        }
        if (KRFFS_IS_DIR(head.mode)) {
            return -EISDIR;
        }
        if (open_at(vol, at)) {
            struct krffs_open *o;

            krffs_stamp_at(&vol->fs, krffs_parent_offset(&vol->fs, at), KRFFS_STAMP_DATA);
            if (krffs_orphan_at(&vol->fs, at) != 0) {
                return -EIO;
            }
            o = open_at(vol, at);
            if (o) {
                o->nameless = 1;
            } else if (kv_reap(vol, at) != 0) {
                return -EIO;
            }
            return 0;
        }
    }
    return krffs_unlink_at(&vol->fs, path) == 0 ? 0 : -EIO;
}

/**
 * A directory `name` in `dir`, with the caller's `mode` and identity on the
 * medium before its name, as `kv_create` makes a file; in a set-group-ID
 * directory it takes the bit as well as the group. A failure in the core
 * answers `made_errno`'s errno, -EINVAL among them for a name whose
 * directory identifier another directory has.
 */
static int kv_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    struct krffs_volume *vol = vol_of(mnt);
    char path[256];
    uint32_t gid, bit;
    int rc;

    if (refuses_writes(vol, mnt)) {
        return -EROFS;
    }
    rc = join(mnt, dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    rc = maker_gid(mnt, dir, &gid, &bit);
    if (rc != 0) {
        return rc;
    }
    rc = krffs_mkdir_at(&vol->fs, path, (uint16_t)((mode & 07777u) | bit), maker_uid(), gid);
    return rc == 0 ? 0 : made_errno(rc);
}

/**
 * Whether the node at `at` is some process's working directory, which the
 * process record keeps as the mount and the node's handle (`vfs_cwd_set`).
 * This type counts no handles, so the working directory takes no hold on
 * the node, and a removal looks for it in the records themselves.
 */
static int cwd_at(struct vfs_mount *mnt, uint64_t at)
{
    const process_t *pr;

    for (pr = proc_list; pr; pr = pr->next) {
        if (pr->fs && pr->fs->cwd_mnt == mnt && pr->fs->cwd_node == handle_of(at)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Remove the directory `name` from `dir`, with the boot root's answers
 * (`rmdir_root_name`): -ENOENT for a name that is not there, -ENOTDIR for
 * one that is not a directory, then -EBUSY and -ENOTEMPTY.
 *
 * A directory a descriptor has open or pinned, or that is a process's
 * working directory, stays, with -EBUSY: this file system has no name for a
 * directory nobody can reach, and freeing it would leave its offset in the
 * allocator's hands while the holder still refers to it. Otherwise 0,
 * -EROFS on a read-only mount, -ENAMETOOLONG, -EIO for a header that cannot
 * be read, or -ENOTEMPTY for any removal that fails in the core.
 */
static int kv_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct krffs_volume *vol = vol_of(mnt);
    char path[256];
    krffs_node_t head;
    uint64_t at;
    int rc;

    if (refuses_writes(vol, mnt)) {
        return -EROFS;
    }
    rc = join(mnt, dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    at = krffs_child_at_err(&vol->fs, offset_of(mnt, dir), name, &rc);
    if (at == KRFFS_NO_NODE) {
        return rc;
    }
    if (krffs_head(&vol->fs, at, &head) != 0) {
        return -EIO;
    }
    if (!KRFFS_IS_DIR(head.mode)) {
        return -ENOTDIR;
    }
    if (open_at(vol, at) || cwd_at(mnt, at)) {
        return -EBUSY;
    }
    return krffs_rmdir_at(&vol->fs, path) == 0 ? 0 : -ENOTEMPTY;
}

/** Whether the volume `arg` has the node at `offset` open, for
 * `krffs_rename_err`. */
static int kv_held(void *arg, uint64_t offset)
{
    return open_at((struct krffs_volume *)arg, offset) != 0;
}

/**
 * Rename `oldname` in `olddir` to `newname` in `newdir`, with the boot
 * root's answers (`rename_root_names`): -ENOENT for a source that is not
 * there, -EPERM for a source that is a directory, whose children refer to
 * it by an identifier made from its name, and -EISDIR for a destination
 * that is one.
 *
 * A rename over a file somebody has open removes it as surely as an unlink
 * does, so the core takes the name off it and keeps the node for whoever
 * has it open (`kv_held`, asked of the destination the core itself
 * found), only once nothing can refuse the rename, and the open record is
 * marked to reap it at the last close. As in `kv_unlink`, the record is
 * looked up again after the writes, which sleep. A rename to the name the
 * file has already does nothing. Otherwise 0, -EROFS on a
 * read-only mount, -ENAMETOOLONG, or -EIO for a header that cannot be read
 * or a rename that failed in the core.
 */
static int kv_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                     const char *newname)
{
    struct krffs_volume *vol = vol_of(mnt);
    char from[256];
    char to[256];
    krffs_node_t head;
    uint64_t orphaned;
    uint64_t src;
    uint64_t dst;
    int rc;

    if (refuses_writes(vol, mnt)) {
        return -EROFS;
    }
    rc = join(mnt, olddir, oldname, from, sizeof(from));
    if (rc != 0) {
        return rc;
    }
    rc = join(mnt, newdir, newname, to, sizeof(to));
    if (rc != 0) {
        return rc;
    }
    src = krffs_child_at_err(&vol->fs, offset_of(mnt, olddir), oldname, &rc);
    if (src == KRFFS_NO_NODE) {
        return rc;
    }
    if (krffs_head(&vol->fs, src, &head) != 0) {
        return -EIO;
    }
    if (KRFFS_IS_DIR(head.mode)) {
        return -EPERM;
    }
    dst = krffs_child_at_err(&vol->fs, offset_of(mnt, newdir), newname, &rc);
    if (dst == KRFFS_NO_NODE && rc == -EIO) {
        return rc;
    }
    if (dst != KRFFS_NO_NODE && dst != src) {
        if (krffs_head(&vol->fs, dst, &head) != 0) {
            return -EIO;
        }
        if (KRFFS_IS_DIR(head.mode)) {
            return -EISDIR;
        }
    }
    rc = krffs_rename_err(&vol->fs, from, to, kv_held, vol, &orphaned);
    if (orphaned != KRFFS_NO_NODE) {
        struct krffs_open *o = open_at(vol, orphaned);

        if (o) {
            o->nameless = 1;
        } else if (kv_reap(vol, orphaned) != 0) {
            return -EIO;
        }
    }
    return rc == 0 ? 0 : -EIO;
}

/**
 * The attributes a volume keeps: the permission bits, the owner and the
 * times, under the mount's lock, with a new change time. The type field of
 * the mode is the node's kind and is not changed, and the root keeps its
 * dirty flag (`krffs_set_mode_at`). Returns 0, -EROFS on a read-only mount,
 * or -EIO for a header that cannot be read or a store that fails, from the
 * medium or because it would change bytes on both sides of a block
 * boundary. The stores before a failed one stay, and the change time is
 * stamped either way.
 */
static int kv_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint64_t at = offset_of(mnt, node);
    krffs_node_t head;
    int rc = begin_write(mnt);

    if (rc != 0) {
        return rc;
    }
    if (krffs_head(&vol->fs, at, &head) != 0) {
        vfs_ns_unlock(mnt);
        return -EIO;
    }
    if (what & VFS_SET_MODE) {
        rc |= krffs_set_mode_at(&vol->fs, at, (uint16_t)st->mode);
    }
    if (what & (VFS_SET_UID | VFS_SET_GID)) {
        rc |= krffs_set_owner_at(&vol->fs, at, st->uid, st->gid,
                                 ((what & VFS_SET_UID) ? KRFFS_SET_UID : 0) |
                                     ((what & VFS_SET_GID) ? KRFFS_SET_GID : 0));
    }
    if (what & VFS_SET_ATIME) {
        rc |= krffs_set_time_at(&vol->fs, at, KRFFS_SET_ATIME, st->atime_sec, st->atime_nsec);
    }
    if (what & VFS_SET_MTIME) {
        rc |= krffs_set_time_at(&vol->fs, at, KRFFS_SET_MTIME, st->mtime_sec, st->mtime_nsec);
    }
    krffs_stamp_at(&vol->fs, at, KRFFS_STAMP_ATTR);
    vfs_ns_unlock(mnt);
    return rc == 0 ? 0 : -EIO;
}

/** The volume's size and free bytes in 1 KiB blocks, KRFFS_FILE_SYSTEM_MAGIC
 * and the longest name; always 0. */
static int kv_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    struct krffs_volume *vol = vol_of(mnt);

    st->magic = KRFFS_FILE_SYSTEM_MAGIC;
    st->bsize = 1024;
    st->blocks = vol->fs.size / 1024u;
    st->bfree = krffs_free_at(&vol->fs) / 1024u;
    st->namelen = KRFFS_FILE_NAME_BUFFER_SIZE - 1;
    return 0;
}

/** Write every dirty buffer of the volume under the mount lock, even after a
 * write failure, but none once the core halted it (`kv_halted`): -EIO. */
static int kv_sync(struct vfs_mount *mnt)
{
    return kv_fsync(mnt, NULL);
}

/**
 * The unmount that wrote the clean flag is not happening after all.
 *
 * The flag says a volume nobody is writing, and this one is still mounted
 * and writable. It goes back on; a volume where that write or its flush
 * fails is one no check would repair after a stop, so the mount takes no
 * more writes.
 */
static void kv_abort_unmount(struct vfs_mount *mnt)
{
    struct krffs_volume *vol = vol_of(mnt);

    if (refuses_writes(vol, mnt)) {
        return;
    }
    if (krffs_set_dirty(&vol->fs, 1) != 0 || bcache_sync(vol->dev) != 0 ||
        bcache_flush(vol->dev) != 0) {
        klog(KLOG_WARN, "krffs",
             "unmount: the volume would not take its dirty flag back: read-only");
        mnt->flags |= VFS_MS_RDONLY;
    }
}

/**
 * Flush the volume, then clear and flush its dirty flag.
 *
 * Earlier writes must reach the medium before the cleared flag can.
 * A mount made read-only, with stale backward links, whose repair left
 * repeated names unsearched, or with an open file that lost its name, as
 * at a shutdown, keeps its dirty flag.
 * If the clear fails, the flag may already be on the device, and the mount
 * takes no more writes. Returns 0, the sync's -errno, or -EIO.
 */
static int kv_prepare_unmount(struct vfs_mount *mnt)
{
    struct krffs_volume *vol = vol_of(mnt);
    int rc = kv_sync(mnt);

    if (rc != 0 || !vol->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return rc;
    }
    if (bcache_flush(vol->dev) != 0) {
        return -EIO;
    }
    if (vol->fs.halted) {
        klog(KLOG_WARN, "krffs", "unmount: a moved file's copy was not withdrawn; repair owed");
        return 0;
    }
    if (vol->fs.stale_links) {
        klog(KLOG_WARN, "krffs", "unmount: backward link repair owed");
        return 0;
    }
    if (vol->fs.check_owed) {
        klog(KLOG_WARN, "krffs", "unmount: a check is owed");
        return 0;
    }
    if (vol->fs.orphans != 0) {
        klog(KLOG_WARN, "krffs", "unmount: nodes without a name; the next mount frees them");
        return 0;
    }
    if (krffs_set_dirty(&vol->fs, 0) != 0) {
        mnt->flags |= VFS_MS_RDONLY;
        klog(KLOG_WARN, "krffs", "unmount: clean flag write failed: read-only");
        return -EIO;
    }
    return 0;
}

/** Free the open records left and the volume, after the unmount has
 * written everything back; the mount has no private state after it. */
static void kv_unmount(struct vfs_mount *mnt)
{
    struct krffs_volume *vol = vol_of(mnt);

    while (vol->opens) {
        struct krffs_open *o = vol->opens;

        vol->opens = o->next;
        kfree(o);
    }
    kfree(vol);
    mnt->priv = 0;
}

/** The canonical path of the directory `node` within the volume; 0, -EIO
 * for a read that fails, or -ENAMETOOLONG for any other path the core
 * cannot make. */
static int kv_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct krffs_volume *vol = vol_of(mnt);
    int rc = krffs_node_path_at(&vol->fs, offset_of(mnt, node), buf, cap);

    return rc == 0 ? 0 : rc == -EIO ? -EIO : -ENAMETOOLONG;
}

static const struct vfs_ops krffs_volume_ops = {
    kv_lookup,
    0, /* lookup_rest */
    0, /* hold: a numeric handle is not counted */
    0, /* put */
    kv_getattr,
    kv_setattr,
    kv_open,
    kv_release,
    kv_read,
    kv_write,
    kv_truncate,
    kv_fsync,
    kv_readdir,
    kv_create,
    kv_unlink,
    kv_mkdir,
    kv_rmdir,
    kv_rename,
    0, /* link: KRFFS has none */
    0, /* readlink: KRFFS has none */
    kv_statfs,
    kv_sync,
    kv_prepare_unmount,
    kv_abort_unmount,
    kv_unmount,
    kv_path,
    0, /* symlink */
    kv_mknod,
    kv_pin,
    kv_unpin,
    0, /* remount */
    0, /* fallocate */
    0, /* fdatasync */
    0, /* lock_node */
    0, /* unlock_node */
    0, /* shutdown */
    0, /* xattr */
    0, /* cachestat */
    0, /* put_unless_last */
    0, /* readlink_cached */
    0, /* read_frames */
    kv_handle_node,
    0, /* writeback */
    0, /* lock_write */
    0, /* unlock_write */
    0, /* rename_flags */
    0, /* tmpfile */
    0, /* uncache */
    kv_phase_of,
    0, /* data_at */
    0, /* check_acl */
};

/**
 * Repair the dirty volume of the writable mount `mnt` (`krffs_repair_at`),
 * and make the mount read-only when the search for repeated names was left
 * unfinished, since a write could act on one of two nodes of one name.
 * 0, or -1 when the repair failed and the mount is refused.
 */
static int kv_repair(struct krffs_volume *vol, struct vfs_mount *mnt)
{
    uint32_t links = 0;
    uint32_t orphans = 0;
    uint32_t doubles = 0;
    int rc;

    klog(KLOG_WARN, "krffs", "check: the volume was not unmounted cleanly");
    bcache_recovering(vol->dev, 1);
    rc = krffs_repair_at(&vol->fs, &links, &orphans, &doubles);
    bcache_recovering(vol->dev, 0);
    if (rc != 0) {
        klog(KLOG_ERR, "krffs", "mount: refused: the volume cannot be repaired here");
        return -1;
    }
    klog(KLOG_WARN, "krffs",
         "check: repaired: %d backward link(s), %d orphan(s), %d repeated name(s)", (int)links,
         (int)orphans, (int)doubles);
    if (vol->fs.check_owed) {
        klog(KLOG_WARN, "krffs", "check: left unfinished; mounted read-only; run fsck.krffs");
        vol->writable = 0;
        mnt->flags |= VFS_MS_RDONLY;
    }
    return 0;
}

/**
 * Mount the KRFFS volume on `dev`.
 *
 * The region is the whole device, since the format records no size of its
 * own: the ring tiles what it was made in, and a device larger than the image
 * fails the chain check rather than mounting short. Every failed mount logs
 * which rule the volume broke.
 *
 * A writable mount of a volume left dirty repairs it first. The backward
 * links are what a stop leaves wrong, and once they are repaired the chain
 * must pass the strict check; a writable mount then sets the dirty flag. A
 * read-only mount does not repair, and its walkers never read a backward
 * link, so its chain need only be walkable. The grain is the cache's block,
 * what the cache puts on the medium at a time, so a write of a field whose
 * changed bytes fall in two blocks fails rather than reach the medium in two
 * pieces. The sector is what the device writes whole: a block reaches it as
 * several sectors, and a stop can fall between them, so a size that
 * publishes a change to the chain is held to one sector.
 *
 * Returns 0; -EINVAL for a volume smaller than two headers, one whose first
 * node lacks the magic number or the root type, or a malformed ring; -EFBIG
 * for a volume of 4 GiB or more; -ENOMEM; or -EIO for a failed header
 * read, repair or dirty flag update. Refusal drops the private buffers
 * before giving up the device claim.
 */
static int krffs_volume_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                              const char *options)
{
    struct krffs_volume *vol;
    uint64_t bytes;
    krffs_node_t root;
    int rc = -EINVAL;

    (void)options;
    bytes = dev->sector_count * (uint64_t)dev->sector_size;
    if (bytes < sizeof(krffs_node_t) * 2) {
        klog(KLOG_ERR, "krffs", "mount: refused: the volume is too small");
        return -EINVAL;
    }
    if (bytes > 0xFFFFFFFFull) {
        klog(KLOG_ERR, "krffs", "mount: refused: the volume is larger than four gibibytes");
        return -EFBIG;
    }

    vol = kzalloc(sizeof(*vol));
    if (!vol) {
        return -ENOMEM;
    }
    vol->dev = dev;
    vol->mnt = mnt;
    vol->fs.backend = &volume_backend;
    vol->fs.ctx = vol;
    vol->fs.size = (uint32_t)bytes;
    vol->fs.grain = BCACHE_BLOCK_BYTES;
    vol->fs.sector = dev->sector_size;
    vol->fs.sector_phase = dev->sector_phase;
    vol->fs.on_halt = kv_halted;
    mnt->priv = vol;

    if (krffs_head(&vol->fs, 0, &root) != 0) {
        klog(KLOG_ERR, "krffs", "mount: refused: the first node cannot be read");
        rc = -EIO;
        goto refused;
    }
    if (root.magic != KRFFS_FILE_SYSTEM_MAGIC) {
        klog(KLOG_ERR, "krffs", "mount: refused: magic 0x%X, expected 0x%X", root.magic,
             KRFFS_FILE_SYSTEM_MAGIC);
        goto refused;
    }
    if (root.type != KRFFS_ROOT_NODE) {
        klog(KLOG_ERR, "krffs", "mount: refused: the first node is type %d, not the root",
             root.type);
        goto refused;
    }
    rc = krffs_chain_walk(&vol->fs, 0);
    if (rc != 1) {
        klog(KLOG_ERR, "krffs", "mount: refused: %s",
             rc < 0 ? "the node chain cannot be read" : "the node chain is malformed");
        goto refused;
    }

    vol->writable = !(flags & VFS_MS_RDONLY) && !(dev->flags & BLOCK_READ_ONLY);
    if (!vol->writable) {
        mnt->flags |= VFS_MS_RDONLY;
        if (root.mode & KRFFS_MODE_DIRTY) {
            klog(KLOG_WARN, "krffs", "mount: a check is owed and this mount is read-only");
        }
    } else if ((root.mode & KRFFS_MODE_DIRTY) && kv_repair(vol, mnt) != 0) {
        rc = -EIO;
        goto refused;
    }

    rc = vol->writable ? krffs_chain_walk(&vol->fs, 1) : 1;
    if (rc != 1) {
        klog(KLOG_ERR, "krffs", "mount: refused: %s",
             rc < 0 ? "the node chain cannot be read" : "the node chain is malformed");
        goto refused;
    }
    if (vol->writable && krffs_set_dirty(&vol->fs, 1) != 0) {
        klog(KLOG_ERR, "krffs", "mount: refused: the volume cannot be written");
        rc = -EIO;
        goto refused;
    }

    mnt->root = handle_of(0);
    klog(KLOG_INFO, "krffs", "mount: %d bytes, %s", (int)vol->fs.size,
         vol->writable ? "read-write" : "read-only");
    return 0;

refused:
    if (vol->writable) {
        /* The constructor still owns the writable device claim. */
        (void)bcache_drop(dev);
    }
    kfree(vol);
    mnt->priv = NULL;
    return rc < 0 ? rc : -EINVAL;
}

const struct vfs_type vfs_krffs_volume_type = {
    "krffs", krffs_volume_mount, &krffs_volume_ops, 1, 0, 0, 0, 0, 0, 0, VFS_DIRECT_ANY};
