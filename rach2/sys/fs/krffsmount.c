/*
 * KRFFS as a mounted volume.
 *
 * The format is in krffs.c, which addresses nodes by their offset from the
 * first byte of the region and reaches the medium through a backend. This
 * file is the other backend, over a block device through the buffer cache,
 * and the VFS type that `mount(2)` places. The root file system is not this:
 * it is a mapped image with a pointer interface, and it never comes through
 * here.
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
 * does, so whoever holds it keeps reading it; the last close frees it. */
struct krffs_open {
    struct krffs_open *next;
    uint64_t offset;
    int refs;
    int nameless; /* unlinked while open: freed at the last close */
};

/* A mounted volume, allocated at mount and freed at unmount. */
struct krffs_volume {
    block_device_t *dev;
    struct vfs_mount *mnt;
    krffs_mount_t fs;
    int writable;
    struct krffs_open *opens;
};

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
 * One buffer is held at a time, which is inside the cache's hold rules.
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
        if (within + take > b->bytes) {
            bcache_release(b); /* past the device's short last block */
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

/*
 * A write goes into the cache and reaches the device at the sync the format
 * asks for, which is what keeps the order a stop must survive: the file
 * system syncs between a write and the field that publishes it.
 */
static int volume_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    struct krffs_volume *vol = ctx;
    const uint8_t *in = buffer;

    if (!vol->writable) {
        return -1;
    }
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
        if (within + take > b->bytes) {
            bcache_release(b); /* past the device's short last block */
            return -1;
        }
        k_memcpy(b->data + within, in, take);
        bcache_dirty(b);
        bcache_release(b);
        in += take;
        offset += take;
        size -= take;
    }
    return 0;
}

/*
 * The format's sync point: the cache's dirty buffers to the device, and then
 * the device's own cache to the medium. A stick answers a write as soon as
 * it has taken it, so a volume whole on the wire is not yet whole on the
 * medium, and the order this file system's protocol rests on would be an
 * order the medium never saw.
 */
static int volume_sync(void *ctx)
{
    struct krffs_volume *vol = ctx;

    if (bcache_sync(vol->dev) != 0) {
        return -1;
    }
    return block_flush(vol->dev) == 0 ? 0 : -1;
}

/*
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
    if (!vol->writable || (mnt->flags & VFS_MS_RDONLY)) {
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
 * put back together here, as the root's adapter does.
 */
static int join(struct vfs_mount *mnt, void *dir, const char *name, char *path, uint32_t cap)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint32_t dlen;
    uint32_t nlen;

    if (krffs_node_path_at(&vol->fs, offset_of(mnt, dir), path, cap) != 0) {
        return -ENAMETOOLONG;
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

static const krffs_backend_t volume_backend = {volume_read, volume_write, volume_sync};

/* The operations. */

static int kv_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct krffs_volume *vol = vol_of(mnt);
    uint64_t at = offset_of(mnt, dir);
    uint64_t found;

    if (name[0] == '.' && name[1] == '\0') {
        *node = handle_of(at);
        return 0;
    }
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        uint64_t parent = krffs_parent_offset(&vol->fs, at);

        *node = handle_of(parent == KRFFS_NO_NODE ? 0 : parent);
        return 0;
    }
    found = krffs_child_at(&vol->fs, at, name);
    if (found == KRFFS_NO_NODE) {
        return -ENOENT;
    }
    *node = handle_of(found);
    return 0;
}

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
        /* The root directory's mode is the format's, not the node's: the
         * root node keeps the dirty flag in the same field. */
        st->mode = KRFFS_S_IFDIR | 0755u;
        st->nlink = 2;
    } else {
        st->mode = KRFFS_IS_DIR(head.mode) ? head.mode : (KRFFS_S_IFREG | (head.mode & 07777u));
        st->nlink = KRFFS_IS_DIR(head.mode) ? 2 : 1;
    }
    st->uid = head.uid;
    st->gid = head.gid;
    /* The offset, unique at any moment; a moved node gets a new number. */
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

/**
 * Release an open record and reap an unlinked file at its last close.
 *
 * Remove the record before reaping, which may sleep under the mount lock.
 */
static void kv_release(struct vfs_mount *mnt, void *handle)
{
    struct krffs_volume *vol = vol_of(mnt);
    struct krffs_open *o = handle;

    if (!o) {
        return;
    }
    if (--o->refs > 0) {
        return;
    }
    {
        uint64_t at = o->offset;
        int reap = o->nameless;
        struct krffs_open **link;

        for (link = &vol->opens; *link != o; link = &(*link)->next) {
        }
        *link = o->next;
        kfree(o);
        if (reap && vol->writable && !(mnt->flags & VFS_MS_RDONLY)) {
            vfs_ns_lock(mnt);
            krffs_remove_at(&vol->fs, at);
            vfs_ns_unlock(mnt);
        }
    }
}

/**
 * Read an open file under the mount lock.
 *
 * The open record retains its identity across relocation and reuse of the
 * old offset, including while the caller waits for the lock.
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

#define KRFFS_DIR_END 0xFFFFFFFFu

/**
 * Read one directory entry under the mount lock through its open handle.
 *
 * The cursor is the next node's offset. Zero starts at the root, which
 * is not an entry; returning to it ends the walk.
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
        if (head.type == KRFFS_RESERVED_NODE && head.parent_id == dir_id && head.name[0] != '\0') {
            uint32_t i;
            uint64_t next = krffs_next_offset(&vol->fs, scan, &head);

            for (i = 0; i + 1 < sizeof(ent->name) && head.name[i] != '\0'; i++) {
                ent->name[i] = (char)head.name[i];
            }
            ent->name[i] = '\0';
            ent->ino = scan + 1;
            ent->is_dir = KRFFS_IS_DIR(head.mode) ? 1 : 0;
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
 * including when the operation fails after relocation.
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
    written = krffs_write_at(&vol->fs, &at, buf, count, (uint32_t)offset);
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
 * including when the operation fails after relocation.
 */
static int kv_truncate(struct vfs_mount *mnt, void *handle, uint64_t size)
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

        rc = krffs_truncate_node_at(&vol->fs, now, (uint32_t)size, &now);
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

/**
 * Write pending buffers under the mount lock, including on a read-only mount.
 *
 * A failed write can make the mount read-only while dirty buffers remain.
 * Return a writeback error to the caller.
 */
static int kv_fsync(struct vfs_mount *mnt, void *handle)
{
    int rc;

    (void)handle;
    vfs_ns_lock(mnt);
    rc = bcache_sync(vol_of(mnt)->dev);
    vfs_ns_unlock(mnt);
    return rc;
}

/**
 * The mode and the owner a creation asks for, put on the node the core made.
 *
 * The core stamps every new node with the format's defaults, which are root's
 * and 0644; the caller's mode has already been masked by its umask, and the
 * identity is the one that made the call. The kind of the node is the core's
 * and is not touched here.
 */
static void stamp_made(struct krffs_volume *vol, uint64_t at, uint32_t mode)
{
    uint32_t uid = current_proc ? current_proc->euid : 0;
    uint32_t gid = current_proc ? current_proc->egid : 0;

    krffs_set_mode_at(&vol->fs, at, (uint16_t)(mode & 07777u));
    krffs_set_owner_at(&vol->fs, at, uid, gid, KRFFS_SET_UID | KRFFS_SET_GID);
}

/**
 * Create `name` in `dir`, with the caller's `mode` and identity.
 *
 * A name that is already there answers -EEXIST. The core would answer
 * success, which would make this a creation of somebody else's file: its
 * mode, its owner and the caller's O_EXCL all belong to the open that finds
 * it.
 */
static int kv_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node)
{
    struct krffs_volume *vol = vol_of(mnt);
    char path[256];
    uint64_t made;
    int rc;

    if (!vol->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    rc = join(mnt, dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    if (krffs_child_at(&vol->fs, offset_of(mnt, dir), name) != KRFFS_NO_NODE) {
        return -EEXIST;
    }
    rc = krffs_create_at(&vol->fs, path);
    if (rc != 0) {
        return -ENOSPC;
    }
    made = krffs_child_at(&vol->fs, offset_of(mnt, dir), name);
    if (made == KRFFS_NO_NODE) {
        return -EIO;
    }
    stamp_made(vol, made, mode);
    *node = handle_of(made);
    return 0;
}

/**
 * Remove `name` from `dir`.
 *
 * A name taken off a file somebody has open leaves the node, which keeps
 * that reader's file alive, and the last close frees it. The open record is
 * looked up again after the writes, which sleep: if a last close freed it
 * meanwhile, the node goes here.
 */
static int kv_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct krffs_volume *vol = vol_of(mnt);
    char path[256];
    int rc;

    if (!vol->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    rc = join(mnt, dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    {
        uint64_t at = krffs_child_at(&vol->fs, offset_of(mnt, dir), name);

        if (at != KRFFS_NO_NODE && open_at(vol, at)) {
            krffs_node_t head;
            struct krffs_open *o;

            if (krffs_head(&vol->fs, at, &head) != 0 || KRFFS_IS_DIR(head.mode)) {
                return -EIO;
            }
            krffs_stamp_at(&vol->fs, krffs_parent_offset(&vol->fs, at), KRFFS_STAMP_DATA);
            krffs_orphan_at(&vol->fs, at);
            o = open_at(vol, at);
            if (o) {
                o->nameless = 1;
            } else {
                krffs_remove_at(&vol->fs, at);
            }
            return 0;
        }
    }
    return krffs_unlink_at(&vol->fs, path) == 0 ? 0 : -EIO;
}

static int kv_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    struct krffs_volume *vol = vol_of(mnt);
    char path[256];
    int rc;

    if (!vol->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    rc = join(mnt, dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    if (krffs_mkdir_at(&vol->fs, path) != 0) {
        return -ENOSPC;
    }
    {
        uint64_t made = krffs_child_at(&vol->fs, offset_of(mnt, dir), name);

        if (made == KRFFS_NO_NODE) {
            return -EIO;
        }
        stamp_made(vol, made, mode);
    }
    return 0;
}

/**
 * Remove the directory `name` from `dir`.
 *
 * A directory somebody has open stays, with -EBUSY: this file system has no
 * name for a directory nobody can reach, and freeing it would leave that
 * holder's offset in the allocator's hands.
 */
static int kv_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct krffs_volume *vol = vol_of(mnt);
    char path[256];
    int rc;

    if (!vol->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return -EROFS;
    }
    rc = join(mnt, dir, name, path, sizeof(path));
    if (rc != 0) {
        return rc;
    }
    {
        uint64_t at = krffs_child_at(&vol->fs, offset_of(mnt, dir), name);

        if (at != KRFFS_NO_NODE && open_at(vol, at)) {
            return -EBUSY;
        }
    }
    return krffs_rmdir_at(&vol->fs, path) == 0 ? 0 : -ENOTEMPTY;
}

/**
 * Rename `oldname` in `olddir` to `newname` in `newdir`.
 *
 * A rename over a file somebody has open removes it as surely as an unlink
 * does, so the name comes off it here and the node stays for whoever has it
 * open, where the core would free it under them. A directory that is open
 * there answers -EISDIR.
 */
static int kv_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                     const char *newname)
{
    struct krffs_volume *vol = vol_of(mnt);
    char from[256];
    char to[256];
    int rc;

    if (!vol->writable || (mnt->flags & VFS_MS_RDONLY)) {
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
    {
        uint64_t dst = krffs_child_at(&vol->fs, offset_of(mnt, newdir), newname);

        if (dst != KRFFS_NO_NODE && open_at(vol, dst)) {
            krffs_node_t head;
            struct krffs_open *o;

            if (krffs_head(&vol->fs, dst, &head) != 0) {
                return -EIO;
            }
            if (KRFFS_IS_DIR(head.mode)) {
                return -EISDIR;
            }
            krffs_stamp_at(&vol->fs, krffs_parent_offset(&vol->fs, dst), KRFFS_STAMP_DATA);
            krffs_orphan_at(&vol->fs, dst);
            /* Looked up again after the writes, which sleep, as in unlink. */
            o = open_at(vol, dst);
            if (o) {
                o->nameless = 1;
            } else {
                krffs_remove_at(&vol->fs, dst);
            }
        }
    }
    return krffs_rename_at(&vol->fs, from, to) == 0 ? 0 : -EIO;
}

/**
 * The attributes a volume keeps: the permission bits, the owner and the
 * times. The type field of the mode is the node's kind and is not changed.
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
    if (at == 0 || krffs_head(&vol->fs, at, &head) != 0) {
        vfs_ns_unlock(mnt);
        return at == 0 ? 0 : -EIO; /* the root's mode is the format's */
    }
    if (what & VFS_SET_MODE) {
        krffs_set_mode_at(&vol->fs, at, (uint16_t)st->mode);
    }
    if (what & (VFS_SET_UID | VFS_SET_GID)) {
        krffs_set_owner_at(&vol->fs, at, st->uid, st->gid,
                           ((what & VFS_SET_UID) ? KRFFS_SET_UID : 0) |
                               ((what & VFS_SET_GID) ? KRFFS_SET_GID : 0));
    }
    if (what & VFS_SET_ATIME) {
        krffs_set_time_at(&vol->fs, at, KRFFS_SET_ATIME, st->atime_sec, st->atime_nsec);
    }
    if (what & VFS_SET_MTIME) {
        krffs_set_time_at(&vol->fs, at, KRFFS_SET_MTIME, st->mtime_sec, st->mtime_nsec);
    }
    krffs_stamp_at(&vol->fs, at, KRFFS_STAMP_ATTR);
    vfs_ns_unlock(mnt);
    return 0;
}

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

/** Write pending buffers under the mount lock, even after a write failure. */
static int kv_sync(struct vfs_mount *mnt)
{
    return kv_fsync(mnt, NULL);
}

/**
 * The unmount that wrote the clean flag is not happening after all.
 *
 * The flag says a volume nobody is writing, and this one is still mounted
 * and writable. It goes back on; a volume that will not take it is one no
 * check would repair after a stop, so the mount takes no more writes.
 */
static void kv_abort_unmount(struct vfs_mount *mnt)
{
    struct krffs_volume *vol = vol_of(mnt);

    if (!vol->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return;
    }
    if (krffs_set_dirty(&vol->fs, 1) != 0 || bcache_sync(vol->dev) != 0 ||
        block_flush(vol->dev) != 0) {
        klog(KLOG_WARN, "krffs",
             "unmount: the volume would not take its dirty flag back: read-only");
        mnt->flags |= VFS_MS_RDONLY;
    }
}

/**
 * Flush the volume, then clear and flush its dirty flag.
 *
 * Earlier writes must reach the medium before the cleared flag can.
 * A mount made read-only before preparation keeps its dirty flag. If the
 * clear fails, the flag may already be on the device; refuse further writes.
 */
static int kv_prepare_unmount(struct vfs_mount *mnt)
{
    struct krffs_volume *vol = vol_of(mnt);
    int rc = kv_sync(mnt);

    if (rc != 0 || !vol->writable || (mnt->flags & VFS_MS_RDONLY)) {
        return rc;
    }
    if (block_flush(vol->dev) != 0) {
        return -EIO;
    }
    if (krffs_set_dirty(&vol->fs, 0) != 0) {
        mnt->flags |= VFS_MS_RDONLY;
        klog(KLOG_WARN, "krffs", "unmount: clean flag write failed: read-only");
        return -EIO;
    }
    return 0;
}

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

static int kv_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct krffs_volume *vol = vol_of(mnt);

    return krffs_node_path_at(&vol->fs, offset_of(mnt, node), buf, cap) == 0 ? 0 : -ENAMETOOLONG;
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
};

/**
 * Mount the KRFFS volume on `dev`.
 *
 * The region is the whole device, since the format records no size of its
 * own: the ring tiles what it was made in, and a device larger than the image
 * fails the chain check rather than mounting short. Every refusal says which
 * rule the volume broke.
 *
 * A writable mount of a volume left dirty repairs it first. The backward
 * links are what a stop leaves wrong, and once they are repaired the chain
 * must pass the strict check; a writable mount then sets the dirty flag. A
 * read-only mount does not repair, and its walkers never read a backward
 * link, so its chain need only be walkable.
 */
static int krffs_volume_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                              const char *options)
{
    struct krffs_volume *vol;
    uint64_t bytes;
    krffs_node_t root;

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
    /* What the cache puts on the medium at a time: a field whose changed
     * bytes fall in two of these is refused rather than published in two
     * pieces (krffs.Readme.md). */
    vol->fs.grain = BCACHE_BLOCK_BYTES;
    mnt->priv = vol;

    if (krffs_head(&vol->fs, 0, &root) != 0) {
        klog(KLOG_ERR, "krffs", "mount: refused: the first node cannot be read");
        kfree(vol);
        mnt->priv = 0;
        return -EIO;
    }
    if (root.magic != KRFFS_FILE_SYSTEM_MAGIC) {
        klog(KLOG_ERR, "krffs", "mount: refused: magic 0x%X, expected 0x%X", root.magic,
             KRFFS_FILE_SYSTEM_MAGIC);
        kfree(vol);
        mnt->priv = 0;
        return -EINVAL;
    }
    if (root.type != KRFFS_ROOT_NODE) {
        klog(KLOG_ERR, "krffs", "mount: refused: the first node is type %d, not the root",
             root.type);
        kfree(vol);
        mnt->priv = 0;
        return -EINVAL;
    }
    if (!krffs_chain_is_walkable(&vol->fs)) {
        klog(KLOG_ERR, "krffs", "mount: refused: the node chain is malformed");
        kfree(vol);
        mnt->priv = 0;
        return -EINVAL;
    }

    vol->writable = !(flags & VFS_MS_RDONLY) && !(dev->flags & BLOCK_READ_ONLY);
    if (!vol->writable) {
        mnt->flags |= VFS_MS_RDONLY;
        if (krffs_is_dirty(&vol->fs)) {
            klog(KLOG_WARN, "krffs", "mount: a check is owed and this mount is read-only");
        }
    } else if (krffs_is_dirty(&vol->fs)) {
        uint32_t links = 0;
        uint32_t orphans = 0;
        uint32_t doubles = 0;

        klog(KLOG_WARN, "krffs", "check: the volume was not unmounted cleanly");
        if (krffs_repair_at(&vol->fs, &links, &orphans, &doubles) != 0) {
            klog(KLOG_ERR, "krffs", "mount: refused: the volume cannot be repaired here");
            kfree(vol);
            mnt->priv = 0;
            return -EIO;
        }
        klog(KLOG_WARN, "krffs",
             "check: repaired: %d backward link(s), %d orphan(s), %d repeated name(s)", (int)links,
             (int)orphans, (int)doubles);
    }

    if (vol->writable && !krffs_chain_is_valid(&vol->fs)) {
        klog(KLOG_ERR, "krffs", "mount: refused: the node chain is malformed");
        kfree(vol);
        mnt->priv = 0;
        return -EINVAL;
    }
    if (vol->writable && krffs_set_dirty(&vol->fs, 1) != 0) {
        klog(KLOG_ERR, "krffs", "mount: refused: the volume cannot be written");
        kfree(vol);
        mnt->priv = 0;
        return -EIO;
    }

    mnt->root = handle_of(0);
    klog(KLOG_INFO, "krffs", "mount: %d bytes, %s", (int)vol->fs.size,
         vol->writable ? "read-write" : "read-only");
    return 0;
}

const struct vfs_type vfs_krffs_volume_type = {"krffs", krffs_volume_mount, &krffs_volume_ops, 1};
