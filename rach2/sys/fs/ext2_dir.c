/*
 * ext2_dir.c: ext2's directories: lookups, listings and paths, entries added
 * and removed, and the operations that change names.
 */

#include "ext2_internal.h"

/* ---- directories ---- */

/* One entry read at a byte position of a directory. */
struct ext2_scan {
    uint32_t ino;
    uint32_t rec_len;
    uint32_t name_len;
    uint32_t type;
    char name[EXT2_NAME_MAX + 1];
    int unread; /* the scan failed at a read of the directory's block */
};

/* A copy of one block of a directory, for a scan that reads all of a block's
 * entries: the block is read once and not twice for each entry. `data` is
 * NULL for a scan without one, and `index` is the copy's block of the
 * directory, or -1 before the first read. */
struct ext2_window {
    uint8_t *data;
    int64_t index;
    int unread; /* the last scan failed at a read of the directory's block */
};

/** Advance the directory's entry version before a write that may change
 * cached bytes even when it returns an error. The caller holds its lock. */
static void dir_changed(struct ext2_node *d)
{
    ext2_assert_node_locked(d);
    d->dir_version++;
}

/** A window for a scan of `em`'s directories; one with no copy, which
 * reads each entry from the cache, when the heap has no room. */
static void window_open(struct ext2_mount *em, struct ext2_window *w)
{
    w->data = kmalloc(em->block_size);
    w->index = -1;
}

/** Free the window's copy. */
static void window_close(struct ext2_window *w)
{
    kfree(w->data);
    w->data = 0;
}

/**
 * Copy `len` bytes at byte `at` of directory `d` into `dst`: through window
 * `w` when it has a copy and the bytes lie inside one block, which a valid
 * entry's do, and by `ext2_read_data` otherwise, for a scan without a copy or a
 * corrupt record whose header runs past its block, a window without a copy
 * as for the latter. 0, or -EIO, with `w->unread` set when a read failed
 * (`ext2_read_data`).
 */
static int dir_bytes(struct ext2_mount *em, const struct ext2_node *d, struct ext2_window *w,
                     uint32_t at, void *dst, uint32_t len)
{
    int64_t index = (int64_t)(at >> em->block_shift);
    int rc;

    if (!w->data || (at & (em->block_size - 1)) + len > em->block_size) {
        return ext2_read_data(em, d, at, dst, len, &w->unread);
    }
    if (w->index != index) {
        w->index = -1;
        rc = ext2_read_data(em, d, (uint64_t)index << em->block_shift, w->data, em->block_size,
                            &w->unread);
        if (rc != 0) {
            return rc;
        }
        w->index = index;
    }
    k_memcpy(dst, w->data + (at & (em->block_size - 1)), len);
    return 0;
}

/**
 * The entry at byte `*cursor` of directory `d`, the cursor moved past it: 1
 * with `sc` filled, 0 at the directory's end, or -EIO. A malformed entry, a
 * `rec_len` below 8 or not a multiple of 4, a record past its block or the
 * directory's size, a name past its record or an inode past the count, and
 * a directory whose size is not a whole number of blocks, as Linux's
 * `ext2_check_folio` refuses them (fs/ext2/dir.c:110-180), are -EIO with
 * the error bit set and a record; a failed read of the directory's block
 * is -EIO with `sc->unread` set and no report, which only a listing makes
 * (`ext2_readdir`). An entry with inode 0 is
 * returned with that inode and no name, and the caller skips it. With window
 * `w` the entry is read from a copy of its block, which the caller must not
 * write while it scans.
 */
static int dir_scan_in(struct ext2_mount *em, const struct ext2_node *d, struct ext2_window *w,
                       uint32_t *cursor, struct ext2_scan *sc)
{
    struct ext2_window none;
    uint8_t head[8];
    uint32_t at = *cursor;
    uint32_t in;
    int rc;

    if (!w) {
        none.data = 0;
        none.index = -1;
        w = &none;
    }
    w->unread = 0;
    sc->unread = 0;
    if (at >= d->size) {
        return 0;
    }
    if (d->size & (em->block_size - 1)) {
        klog(KLOG_WARN, "ext2", "directory %d: size %d is not a whole number of blocks",
             (int)d->ino, (int)d->size);
        ext2_mark_error(em);
        return -EIO;
    }
    in = at % em->block_size;
    rc = dir_bytes(em, d, w, at, head, 8);
    if (rc != 0) {
        sc->unread = w->unread;
        return rc;
    }
    sc->ino = ext2_le32(head);
    sc->rec_len = ext2_le16(head + 4);
    sc->name_len = head[6];
    sc->type = head[7];
    if (sc->rec_len < 8 || sc->rec_len % 4 != 0 || in + sc->rec_len > em->block_size ||
        (uint64_t)at + sc->rec_len > d->size || sc->name_len + 8 > sc->rec_len ||
        sc->ino > em->inodes) {
        klog(KLOG_WARN, "ext2", "directory %d: entry at %d is malformed", (int)d->ino, (int)at);
        ext2_mark_error(em);
        return -EIO;
    }
    if (sc->ino != 0) {
        rc = dir_bytes(em, d, w, at + 8, sc->name, sc->name_len);
        if (rc != 0) {
            sc->unread = w->unread;
            return rc;
        }
    } else {
        sc->name_len = 0;
    }
    sc->name[sc->name_len] = '\0';
    *cursor = at + sc->rec_len;
    return 1;
}

/** `dir_scan_in` without a window: each entry read through the cache. */
static int dir_scan(struct ext2_mount *em, const struct ext2_node *d, uint32_t *cursor,
                    struct ext2_scan *sc)
{
    return dir_scan_in(em, d, 0, cursor, sc);
}

/** Refuse directory inode `ino`, damaged as `why` says: logged, reported to
 * the mount's policy (`ext2_mark_error`) and -EIO. The caller holds no
 * buffer. */
static int dir_damaged(struct ext2_mount *em, uint32_t ino, const char *why)
{
    klog(KLOG_WARN, "ext2", "directory %d: %s", (int)ino, why);
    ext2_mark_error(em);
    return -EIO;
}

/** Whether the strings `a` and `b` are equal. */
static int names_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * Find `name` in directory `d`, whose lock the caller holds, for a lookup:
 * 1 with the inode it names in `*ino` and the entry's byte in `*at`, 0 when
 * it is absent, or -EIO.
 *
 * A directory of this format has no index, so the lookup reads entries
 * until it finds the name. It starts at the block where the last lookup in
 * the same directory found its name (`lookup_from`) and wraps around to it,
 * as Linux's ext2 does (fs/ext2/dir.c:356-395): `ls` and `find` look up the
 * names of a directory in the order it lists them, so each is found near
 * the one before, and a directory of n names is listed in time proportional
 * to n and not to n squared. A block starts an entry whatever was written
 * since, since no entry crosses one.
 */
static int dir_lookup(struct ext2_mount *em, struct ext2_node *d, const char *name, uint32_t *ino,
                      uint32_t *at)
{
    uint32_t start = 0, cursor, limit;
    struct ext2_window w;
    int pass;

    if (((uint64_t)d->lookup_from << em->block_shift) < d->size) {
        start = d->lookup_from << em->block_shift;
    }
    window_open(em, &w);
    for (pass = 0; pass < 2; pass++) {
        cursor = pass == 0 ? start : 0;
        limit = pass == 0 ? (uint32_t)d->size : start;
        while (cursor < limit) {
            struct ext2_scan sc;
            int got;

            *at = cursor;
            got = dir_scan_in(em, d, &w, &cursor, &sc);
            if (got < 0) {
                window_close(&w);
                return got;
            }
            if (got == 0) {
                break;
            }
            if (sc.ino != 0 && names_equal(sc.name, name)) {
                d->lookup_from = *at >> em->block_shift;
                window_close(&w);
                *ino = sc.ino;
                return 1;
            }
        }
    }
    window_close(&w);
    return 0;
}

/** Whether the entry at byte `at` of directory `d` still names inode `ino`
 * as `name`, read under `d`'s lock: 1, 0, or -EIO. A changed `version`
 * returns 0 before parsing an offset that a removed record may cover. */
static int dir_names_at(struct ext2_mount *em, struct ext2_node *d, uint32_t at, const char *name,
                        uint32_t ino, uint64_t version)
{
    struct ext2_scan sc;
    uint32_t cursor = at;
    int got;

    ext2_node_lock(d);
    got = d->dir_version == version ? dir_scan(em, d, &cursor, &sc) : 0;
    ext2_node_unlock(d);
    return got <= 0 ? got : sc.ino == ino && names_equal(sc.name, name);
}

/**
 * The `lookup` of `vfs_ops`: `name` in directory `dir`, the mount's root when
 * it is NULL. 0 with the node held, not locked; -ENOTDIR, -ENOENT, -ENOMEM,
 * or -EIO.
 *
 * The entry is found under the directory's lock, which the caller may hold
 * already. The first try has the node with the lock let go, so that the read
 * of a cold inode holds up no other lookup in the directory and a second
 * lookup of the name waits on the load itself (`ext2_node_for`); the entry is
 * then read again under the lock, and any change of the directory meanwhile,
 * since it may have removed or moved the name and freed its inode or given it
 * to another file, sends the lookup to a second try. That one holds the lock
 * from the search until it has the node, as Linux's lookup holds the
 * directory's `i_rwsem` across `ext2_lookup` and `ext2_iget` (fs/namei.c:1761,
 * fs/ext2/namei.c:71), so changes of other names in a busy directory cannot
 * send it round for ever. A freed inode the first try finds sends it to the
 * second too; one found with the lock held is damage, reported as Linux's
 * `ext2_lookup` reports `deleted inode referenced` (`ext2_stale`). The node
 * table's big lock, which `ext2_node_for` takes, comes after a node's lock,
 * and the load takes no node lock. A fenced directory (`d->fenced`) is -EIO,
 * found under its lock, as Linux's bad inode answers a lookup.
 */
int ext2_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    uint32_t ino = 0, at = 0;
    uint64_t version;
    int rc, kept, held;

    if ((d->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    if (name[0] == '.' && name[1] == '\0') {
        ext2_node_ref(d, 1);
        *node = d;
        return 0;
    }
    for (held = 0;; held = 1) {
        ext2_node_lock(d);
        rc = d->fenced ? -EIO : dir_lookup(em, d, name, &ino, &at);
        version = d->dir_version;
        if (rc > 0 && ino == d->ino) {
            ext2_node_ref(d, 1);
            ext2_node_unlock(d);
            *node = d;
            return 0;
        }
        if (rc <= 0 || !held) {
            ext2_node_unlock(d);
        }
        if (rc <= 0) {
            return rc < 0 ? rc : -ENOENT;
        }
        kept = held || sleep_lock_held(&d->lock);
        n = ext2_node_for(em, ino, &rc);
        if (held) {
            ext2_node_unlock(d);
        }
        if (!n && rc == -ESTALE && !kept) {
            continue;
        }
        if (!n) {
            return rc == -ESTALE ? ext2_stale(em, ino) : rc;
        }
        rc = kept ? 1 : dir_names_at(em, d, at, name, ino, version);
        if (rc == 1) {
            *node = n;
            return 0;
        }
        ext2_put(mnt, n);
        if (rc < 0) {
            return rc;
        }
    }
}

/**
 * The getdents64 type of the file-type byte a directory entry stores under
 * the filetype feature: regular file, directory, character and block
 * device, FIFO, socket and symbolic link are 1 through 7 on disk, and 0 or a
 * value past EXT2_FT_MAX is VFS_DT_UNKNOWN.
 */
static int ext2_dtype(uint32_t ft)
{
    static const unsigned char dt[EXT2_FT_MAX + 1] = {0, 8, 4, 2, 6, 1, 12, 10};

    return ft <= EXT2_FT_MAX ? dt[ft] : VFS_DT_UNKNOWN;
}

/**
 * The entry after byte `*cursor` of directory `d` that names an inode, for
 * `ext2_readdir`, whose caller holds `d`'s lock: 1 with `ent` filled and the
 * cursor past it, 0 at the end, -ENOENT for a removed directory, or -EIO,
 * with `*unread` set for a failed read of a block (`dir_scan_in`). Without
 * the filetype feature an entry's inode is loaded for its type, and one
 * that is freed is reported (`ext2_stale`).
 */
static int dir_next(struct ext2_mount *em, struct ext2_node *d, uint32_t *cursor,
                    struct vfs_dirent *ent, int *unread)
{
    if (d->unlinked) {
        return -ENOENT;
    }
    for (;;) {
        struct ext2_scan sc;
        int got = dir_scan(em, d, cursor, &sc);

        if (got <= 0) {
            *unread = sc.unread;
            return got;
        }
        if (sc.ino == 0) {
            continue;
        }
        /* name_len bytes and the NUL, as Linux's dir_emit, fs/ext2/dir.c:311 */
        k_memcpy(ent->name, sc.name, sc.name_len + 1);
        ent->ino = sc.ino;
        if (em->incompat & EXT2_INCOMPAT_FILETYPE) {
            ent->is_dir = sc.type == EXT2_FT_DIR;
            ent->type = ext2_dtype(sc.type);
        } else {
            struct ext2_node probe;
            int rc = ext2_inode_load(em, sc.ino, &probe);

            if (rc != 0) {
                return rc == -ESTALE ? ext2_stale(em, sc.ino) : rc;
            }
            ent->is_dir = (probe.mode & S_IFMT_) == S_IFDIR_;
            ent->type = (int)VFS_DT_OF(probe.mode);
        }
        return 1;
    }
}

/**
 * Move `*cursor` to the first current record boundary at or after it, from
 * the start of its block, under `d`'s lock. Returns 0 or -EIO, with
 * `*unread` set for a failed read of a block. A removal can merge the saved
 * offset into a name written later in that space; only the block's current
 * boundaries may be parsed as headers.
 */
static int dir_realign(struct ext2_mount *em, struct ext2_node *d, uint32_t *cursor, int *unread)
{
    uint32_t at = *cursor & ~(em->block_size - 1u);
    struct ext2_scan sc;
    int got;

    if (*cursor >= d->size) {
        return 0;
    }
    while (at < *cursor) {
        got = dir_scan(em, d, &at, &sc);
        if (got < 0) {
            *unread = sc.unread;
            return got;
        }
        if (got == 0) {
            break;
        }
    }
    *cursor = at;
    return 0;
}

/**
 * The `readdir` of `vfs_ops`: the next entry with an inode of directory
 * `handle` at `*cursor`, a byte offset, `.` and `..` included. Its type comes
 * from the entry's file-type byte when the volume has the filetype feature,
 * and from the inode's mode otherwise, which costs a read of the inode. 1
 * with `ent` filled, 0 at the end, -ENOTDIR, -ENOENT for a directory removed
 * while held, or -EIO. Under the directory's lock, a version mismatch or
 * a seek realigns the cursor from its block's start before `dir_next`. A
 * block that cannot be read is logged and reported to the mount's policy
 * (`ext2_mark_error`), as Linux's `ext2_readdir` reports a bad page
 * (fs/ext2/dir.c:282); a lookup's failed read is not, as Linux's is not.
 * A fenced directory (`d->fenced`) is -EIO, found under its lock.
 */
int ext2_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent)
{
    struct ext2_handle *h = handle;
    struct ext2_node *d = h->node;
    struct ext2_mount *em = ext2_em_of(mnt);
    int rc, unread = 0;

    if ((d->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    ext2_node_lock(d);
    rc = d->fenced ? -EIO : d->unlinked ? -ENOENT : 0;
    if (rc == 0 && (h->dir_version != d->dir_version || h->cursor != *cursor)) {
        rc = dir_realign(em, d, cursor, &unread);
    }
    if (rc == 0) {
        rc = dir_next(em, d, cursor, ent, &unread);
    }
    if (rc == -EIO && unread) {
        klog(KLOG_WARN, "ext2", "directory %d: a block cannot be read", (int)d->ino);
        ext2_mark_error(em);
    }
    if (rc >= 0) {
        h->dir_version = d->dir_version;
        h->cursor = *cursor;
    }
    ext2_node_unlock(d);
    return rc;
}

/** Refuse symbolic link `n`, damaged as `why` says: logged, reported to the
 * mount's policy (`ext2_mark_error`) and -EIO. The caller holds no buffer. */
static int link_damaged(struct ext2_mount *em, const struct ext2_node *n, const char *why)
{
    klog(KLOG_WARN, "ext2", "symbolic link %d: %s", (int)n->ino, why);
    ext2_mark_error(em);
    return -EIO;
}

/**
 * The target of symbolic link `n` into `buf`, as `ext2_readlink` gives it,
 * for a caller that holds `n`'s lock, or a hold alone on a fast link
 * (`fast_target`), whose words do not change. A size of a block or more, a
 * fast link of 60 bytes or more, and a long link without its block are
 * damage, refused by `link_damaged`; `ext2_readlink_cached` passes none of
 * them here.
 */
static int link_target(struct ext2_mount *em, const struct ext2_node *n, char *buf, uint32_t cap)
{
    uint32_t len;

    if (n->size >= em->block_size) {
        return link_damaged(em, n, "its size is a block or more");
    }
    len = (uint32_t)n->size;
    if (len + 1 > cap) {
        return -ENAMETOOLONG;
    }
    if (n->fast_target) {
        uint32_t i;

        if (len >= EXT2_FAST_LINK) {
            /* the pointer words hold at most 59 bytes */
            return link_damaged(em, n, "a fast link of 60 bytes or more");
        }
        for (i = 0; i < len; i++) {
            buf[i] = (char)((n->block[i / 4] >> ((i % 4) * 8)) & 0xFFu);
        }
    } else {
        int rc;

        if (n->block[0] == 0) {
            return link_damaged(em, n, "a long link without its block");
        }
        rc = ext2_read_data(em, n, 0, buf, len, 0);
        if (rc != 0) {
            return rc;
        }
    }
    buf[len] = '\0';
    return (int)len;
}

/**
 * The `readlink` of `vfs_ops`: a symbolic link's target, from the inode's
 * pointer words when it is shorter than sixty bytes and the inode has no
 * blocks, from its first block otherwise, with a terminating NUL. Returns
 * the length; -ENAMETOOLONG when `cap` cannot hold it; -EINVAL for a node
 * that is not a link; or -EIO for a size of a block or more, a link of 60
 * bytes or more without blocks, a long link without its block, or a failed
 * read, or for a fenced link (`n->fenced`), found under the lock. Takes the
 * link's lock (`link_target`), as a read does.
 */
int ext2_readlink(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct ext2_node *n = node;
    int rc;

    if ((n->mode & S_IFMT_) != S_IFLNK_) {
        return -EINVAL;
    }
    ext2_node_lock(n);
    rc = n->fenced ? -EIO : link_target(ext2_em_of(mnt), n, buf, cap);
    ext2_node_unlock(n);
    return rc;
}

/**
 * The `readlink_cached` of `vfs_ops`: the target of a fast link, which the
 * inode's pointer words hold, as Linux's ext2 points `i_link` at them
 * (fs/ext2/inode.c:1488-1491, namei.c:186-190); -1 for any other node, a
 * slow link among them, or a `cap` too small. Whether the link is fast is
 * `fast_target`, decided when the inode was read or the link made, since
 * `ext2_fast_link` reads `blocks` and `file_acl`, which a setxattr changes in
 * two stores under the big lock that this walk does not take. A link's mode,
 * size and words do not change while a hold keeps it, so this takes no
 * lock. A fenced link (`n->fenced`) is -1, which sends the caller to
 * `ext2_readlink`, which refuses it.
 */
int ext2_readlink_cached(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = node;

    if (n->fenced || !n->fast_target || n->size >= EXT2_FAST_LINK || n->size + 1 > cap) {
        return -1;
    }
    return link_target(em, n, buf, cap);
}

/* One directory of the bounded walk for a regular file's current name. */
struct ext2_path_level {
    uint32_t ino, cursor, length;
};

/**
 * The entry of directory `d`, whose lock the caller holds, that names inode
 * `ino` by a name other than `.` and `..`, into `sc`: 1, 0 when it has none,
 * or -EIO.
 */
static int dir_name_of(struct ext2_mount *em, const struct ext2_node *d, uint32_t ino,
                       struct ext2_scan *sc)
{
    uint32_t cursor = 0;

    for (;;) {
        int got = dir_scan(em, d, &cursor, sc);

        if (got <= 0) {
            return got;
        }
        if (sc->ino == ino && !names_equal(sc->name, ".") && !names_equal(sc->name, "..")) {
            return 1;
        }
    }
}

/** Whether node `d`, whose lock the caller holds, is a directory with a
 * name, whose entries may be read: one freed and given to another file
 * after a walk read its number is not. */
static int dir_live(const struct ext2_node *d)
{
    return (d->mode & S_IFMT_) == S_IFDIR_ && !d->unlinked;
}

/**
 * One step of the walk for a file's name: the next entry of directory
 * inode `ino` after byte `*cursor`, into `sc`, read under the directory's
 * lock with the node held, and whether the inode it names is a directory,
 * into `*is_dir`. 1, 0 at the directory's end, or -errno. The lock is let
 * go before the next step, so the walk holds one directory at a time, and a
 * directory freed since its number was read has no entries (`dir_live`),
 * as one given to another file has not.
 */
static int path_step(struct ext2_mount *em, uint32_t ino, uint32_t *cursor, struct ext2_scan *sc,
                     int *is_dir)
{
    int got;
    struct ext2_node *d;

    *is_dir = 0;
    d = ext2_node_for(em, ino, &got);
    if (!d) {
        return got == -ESTALE ? 0 : got;
    }
    ext2_node_lock(d);
    got = dir_live(d) ? dir_scan(em, d, cursor, sc) : 0;
    if (got > 0 && sc->ino != 0) {
        if (em->incompat & EXT2_INCOMPAT_FILETYPE) {
            *is_dir = sc->type == EXT2_FT_DIR;
        } else {
            struct ext2_node child;

            got = ext2_inode_load(em, sc->ino, &child);
            got = got == -ESTALE ? ext2_stale(em, sc->ino) : got != 0 ? got : 1;
            *is_dir = got == 1 && (child.mode & S_IFMT_) == S_IFDIR_;
        }
    }
    ext2_node_unlock(d);
    ext2_put(em->mnt, d);
    return got;
}

/** Find one live name of `wanted` below the root. Directories have no hard
 * links except . and .., which are not descended. The stack is on the heap,
 * bounded by the path capacity, and no recursive kernel stack is used; the
 * walk holds one directory's lock at a time (`path_step`). */
static int ext2_file_path(struct ext2_mount *em, uint32_t wanted, char *buf, uint32_t cap)
{
    struct ext2_path_level *levels;
    struct ext2_scan sc;
    uint32_t depth = 0, limit = cap / 2;
    int rc = -ENOENT, clipped = 0;

    if (limit == 0) {
        return -ENAMETOOLONG;
    }
    if (limit > PATH_DEPTH) {
        limit = PATH_DEPTH;
    }
    levels = kmalloc(limit * sizeof(*levels));
    if (!levels) {
        return -ENOMEM;
    }
    k_memset(levels, 0, limit * sizeof(*levels));
    levels[0].ino = EXT2_ROOT_INO;
    buf[0] = '\0';
    for (;;) {
        struct ext2_path_level *at = &levels[depth];
        uint32_t length = at->length;
        int got, is_dir;

        got = path_step(em, at->ino, &at->cursor, &sc, &is_dir);
        if (got < 0) {
            rc = got;
            break;
        }
        if (got == 0) {
            if (depth == 0) {
                rc = clipped ? -ENAMETOOLONG : -ENOENT;
                break;
            }
            depth--;
            continue;
        }
        if (sc.ino == 0 || names_equal(sc.name, ".") || names_equal(sc.name, "..")) {
            continue;
        }
        if (sc.ino == wanted) {
            if (length + sc.name_len + 1 >= cap) {
                rc = -ENAMETOOLONG;
                break;
            }
            buf[length] = '/';
            k_memcpy(buf + length + 1, sc.name, sc.name_len);
            buf[length + sc.name_len + 1] = '\0';
            rc = 0;
            break;
        }
        if (!is_dir) {
            continue;
        }
        if (length + sc.name_len + 1 >= cap || depth + 1 == limit) {
            clipped = 1;
            continue;
        }
        buf[length] = '/';
        k_memcpy(buf + length + 1, sc.name, sc.name_len);
        buf[length + sc.name_len + 1] = '\0';
        depth++;
        levels[depth].ino = sc.ino;
        levels[depth].cursor = 0;
        levels[depth].length = length + sc.name_len + 1;
    }
    kfree(levels);
    return rc;
}

/**
 * The parent of directory `cur`, held, into `*parent`, by its `..` entry,
 * read under `cur`'s lock; the node is had with the lock let go, as a
 * lookup has it, and the caller checks under the parent's own lock that it
 * is a directory still. 0, -ENOENT for a removed directory or one whose
 * parent was freed after the entry was read, -EIO for a live directory
 * without `..`, which is damage (`dir_damaged`), or -errno.
 */
static int dir_parent(struct ext2_mount *em, struct ext2_node *cur, struct ext2_node **parent)
{
    struct ext2_scan sc;
    uint32_t cursor = 0;
    int rc = 0, gone;

    ext2_node_lock(cur);
    while (!cur->unlinked) {
        rc = dir_scan(em, cur, &cursor, &sc);
        if (rc <= 0 || (sc.ino != 0 && names_equal(sc.name, ".."))) {
            break;
        }
    }
    gone = cur->unlinked;
    ext2_node_unlock(cur);
    if (rc == 0 && !gone) {
        return dir_damaged(em, cur->ino, "it has no '..'");
    }
    if (rc == 1) {
        *parent = ext2_node_for(em, sc.ino, &rc);
        if (rc == -ESTALE) {
            rc = -ENOENT;
        }
    } else if (rc == 0) {
        rc = -ENOENT;
    }
    return rc;
}

/**
 * The `path` of `vfs_ops`: a regular file uses the bounded directory walk
 * above. A directory finds the parent by its `..` entry and the name by the
 * parent's entry that names the child's inode, up to the root, with one
 * directory's lock held at a time and each node on the way held. 0;
 * -ENAMETOOLONG past `cap` or PATH_DEPTH levels; -ENOENT without a live
 * name; -ENOMEM for the walk's stack; or the directory reader's error.
 */
int ext2_path(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = node;
    struct ext2_node *cur = n;
    uint32_t len = 0;
    int depth, rc = 0;

    if ((n->mode & S_IFMT_) != S_IFDIR_) {
        return n->nlink ? ext2_file_path(em, n->ino, buf, cap) : -ENOENT;
    }
    buf[0] = '\0';
    for (depth = 0; cur->ino != EXT2_ROOT_INO && depth < PATH_DEPTH; depth++) {
        struct ext2_node *parent = 0;
        struct ext2_scan sc;
        uint32_t child = cur->ino;

        rc = dir_parent(em, cur, &parent);
        if (cur != n) {
            ext2_put(mnt, cur);
        }
        cur = parent;
        if (rc != 0) {
            break;
        }
        ext2_node_lock(cur);
        rc = dir_live(cur) ? dir_name_of(em, cur, child, &sc) : 0;
        ext2_node_unlock(cur);
        if (rc <= 0) {
            rc = rc < 0 ? rc : -ENOENT;
            break;
        }
        rc = 0;
        if (len + sc.name_len + 1 >= cap) {
            rc = -ENAMETOOLONG;
            break;
        }
        k_memmove(buf + sc.name_len + 1, buf, len + 1);
        buf[0] = '/';
        k_memcpy(buf + 1, sc.name, sc.name_len);
        len += sc.name_len + 1;
    }
    if (cur && cur != n) {
        ext2_put(mnt, cur);
    }
    if (rc != 0) {
        return rc;
    }
    if (depth == PATH_DEPTH) {
        return -ENAMETOOLONG;
    }
    if (len == 0) {
        buf[0] = '/';
        buf[1] = '\0';
    }
    return 0;
}

/* ---- directories, written ---- */

/** The record length of an entry with a name of `name_len` bytes: the
 * 8-byte header and the name, rounded up to 4. */
static uint32_t dirent_len(uint32_t name_len)
{
    return (8 + name_len + 3) & ~3u;
}

/** The file-type byte of an entry for an inode of mode `mode`, 1 to 7, or 0
 * for a kind the format has no type for. */
uint32_t ext2_ftype_of(uint32_t mode)
{
    switch (mode & S_IFMT_) {
    case S_IFREG_:
        return 1;
    case S_IFDIR_:
        return 2;
    case S_IFCHR_:
        return 3;
    case S_IFBLK_:
        return 4;
    case S_IFIFO_:
        return 5;
    case S_IFSOCK_:
        return 6;
    case S_IFLNK_:
        return 7;
    default:
        return 0;
    }
}

/** Find `name` in `d`: 1 with the entry in `sc`, its position in `*at` and
 * its predecessor's in the same block in `*prev_at`, or the position itself
 * when it is first in its block; 0 when absent, or -EIO. */
static int dir_find(struct ext2_mount *em, const struct ext2_node *d, const char *name,
                    uint32_t *at, uint32_t *prev_at, struct ext2_scan *sc)
{
    uint32_t cursor = 0, prev = 0;
    struct ext2_window w;
    int got;

    window_open(em, &w);
    for (;;) {
        uint32_t here = cursor;

        got = dir_scan_in(em, d, &w, &cursor, sc);
        if (got <= 0) {
            break;
        }
        if (here % em->block_size == 0) {
            prev = here;
        }
        if (sc->ino != 0 && names_equal(sc->name, name)) {
            *at = here;
            *prev_at = prev;
            break;
        }
        prev = here;
    }
    window_close(&w);
    return got;
}

/** The byte offset on the volume of byte `at` of directory `d`, through its
 * map, into `*off`; 0, -EIO for a hole inside the size, which is damage
 * (`dir_damaged`), as Linux's `ext2_find_entry` reports a size past the
 * blocks (fs/ext2/dir.c:384), or the errors of `ext2_block_at`. */
static int dir_block_at(struct ext2_mount *em, const struct ext2_node *d, uint32_t at,
                        uint64_t *off)
{
    uint32_t b;
    int rc = ext2_block_at(em, d, at / em->block_size, &b);

    if (rc != 0) {
        return rc;
    }
    if (b == 0) {
        return dir_damaged(em, d->ino, "a hole inside its size");
    }
    *off = (uint64_t)b * em->block_size + at % em->block_size;
    return 0;
}

/** Encode the entry naming `ino`, its header, name and padding, at `raw`,
 * with the record length `rec_len` and the type byte `type` as the volume
 * keeps it; returns the entry's own length. */
static uint32_t dirent_encode(uint8_t *raw, uint32_t ino, uint32_t rec_len, const char *name,
                              uint32_t name_len, uint32_t type)
{
    uint32_t len = dirent_len(name_len);

    k_memset(raw, 0, len);
    LE32_PUT(raw, ino);
    LE16_PUT(raw + 4, rec_len);
    raw[6] = (uint8_t)name_len;
    raw[7] = (uint8_t)type;
    k_memcpy(raw + 8, name, name_len);
    return len;
}

/** Write the entry naming `ino`, with the record length `rec_len`, at byte
 * `off` of the volume with WRITE_THROUGH; its type byte is `ftype` when the
 * volume has the filetype feature and 0 otherwise. 0, or -errno. */
static int dirent_write(struct ext2_mount *em, uint64_t off, uint32_t ino, uint32_t rec_len,
                        const char *name, uint32_t name_len, uint32_t ftype)
{
    uint8_t raw[8 + EXT2_NAME_MAX + 4]; /* the header, the name and its padding */
    uint32_t type = (em->incompat & EXT2_INCOMPAT_FILETYPE) ? ftype : 0;
    uint32_t len = dirent_encode(raw, ino, rec_len, name, name_len, type);

    return ext2_write_bytes(em, off, raw, len, WRITE_THROUGH | WRITE_KIND(READ_DATA));
}

/**
 * Split the entry `old` at `off` for the new entry `entry`, of `len` bytes
 * with the rest of `old`'s record, which goes behind `old`'s name. 0, or
 * -errno.
 *
 * A sector is the unit the disk writes whole, so when the two headers and
 * the new name lie in one, one write does both. Otherwise the new entry
 * is written into the slack behind the old name first, where the old
 * record still covers it, and then the old header is shortened to end
 * where the new one begins; a stop between the two leaves the old entry
 * spanning its record and the new one unreachable, which is the volume as
 * it was.
 */
static int dirent_split(struct ext2_mount *em, uint64_t off, const struct ext2_scan *old,
                        const uint8_t *entry, uint32_t len)
{
    uint8_t pair[2 * (8 + EXT2_NAME_MAX + 4)];
    uint32_t used = dirent_len(old->name_len);
    int rc;

    if ((off >> 9) == ((off + used + len - 1) >> 9)) {
        dirent_encode(pair, old->ino, used, old->name, old->name_len, old->type);
        k_memcpy(pair + used, entry, len);
        return ext2_write_bytes(em, off, pair, used + len, WRITE_THROUGH | WRITE_KIND(READ_DATA));
    }
    rc = ext2_write_bytes(em, off + used, entry, len, WRITE_THROUGH | WRITE_KIND(READ_DATA));
    if (rc != 0) {
        return rc;
    }
    used = dirent_encode(pair, old->ino, used, old->name, old->name_len, old->type);
    return ext2_write_bytes(em, off, pair, used, WRITE_THROUGH | WRITE_KIND(READ_DATA));
}

/**
 * Write bytes [off, off + len) of the volume, a directory's entries just
 * changed, to the device now on a mount that writes back, as Linux's
 * `ext2_handle_dirsync` writes a directory's pages after every change of
 * its entries, with no test of `dirsync` (fs/ext2/dir.c:440-447, called at
 * 470, 557, 611 and 650 in Linux 6.12.115). So a removed name is on the
 * device before the inode it named loses its count, is freed and is given
 * to another file, and a new name, a pointed entry or a new `..` before
 * the operation's next step. A strict mount wrote the bytes through
 * already. 0; -EROFS without a write once a write on the device has
 * failed, as a strict mount's next write is, so that an operation in
 * progress stops there; or the write's -errno, with the block still dirty.
 *
 * As in Linux, the disk's cache is not flushed. Linux next writes the
 * directory's inode; here `inode_commit` writes it where a later step
 * depends on it and when a mkdir raised its count, and otherwise it writes
 * back, since a stop that loses what is left, its times or another count,
 * loses no name, and the mount's check sets a count to its names.
 */
static int dir_commit(struct ext2_mount *em, uint64_t off, uint32_t len)
{
    uint64_t blk = off >> BCACHE_BLOCK_SHIFT;
    uint64_t end = (off + len - 1) >> BCACHE_BLOCK_SHIFT;
    int rc = 0;

    if (ext2_em_strict(em)) {
        return 0;
    }
    if (em->dev->flags & BLOCK_WRITE_FAILED) {
        return -EROFS;
    }
    for (; rc == 0 && blk <= end; blk++) {
        rc = bcache_sync_block(em->dev, blk);
    }
    return rc;
}

/**
 * Write the table block that holds inode `n`, which the caller has just
 * stored, to the device now on a mount that writes back, as Linux's
 * `ext2_handle_dirsync` follows a directory's pages with
 * `sync_inode_metadata` (fs/ext2/dir.c:440-447 in Linux 6.12.115), for a
 * step that depends on the inode: a grown directory's new pointer, size and
 * `i_blocks` before a name in the new block lets another go, a cleared
 * index flag before the entries change, and a new directory and its
 * parent's raised count before the parent names it. The cache writes the
 * block after the dirty blocks it may name (`bcache_sync_last`,
 * `table_names`): the shared blocks dirtied before its last mark that are
 * not `after`, such as bitmaps, descriptors and directory and pointer
 * blocks, and the blocks of a regular file beside it in the table block
 * that allocated or freed since its last write. Those are what the write
 * costs past its own request: a directory's own blocks were written as
 * they changed (`dir_commit`). A strict mount wrote the inode through
 * already. 0; -EROFS without a write once a write on the device has
 * failed; or the write's -errno, with the block still dirty.
 */
static int inode_commit(struct ext2_mount *em, const struct ext2_node *n)
{
    if (ext2_em_strict(em)) {
        return 0;
    }
    if (em->dev->flags & BLOCK_WRITE_FAILED) {
        return -EROFS;
    }
    return bcache_sync_block(em->dev, ext2_inode_offset(em, n->ino) >> BCACHE_BLOCK_SHIFT);
}

/**
 * Write the new block at byte `off` of the volume, a directory's or a
 * symlink's target's, zeroed, with `head`, its first `len` bytes, over the
 * zeros, and on a mount that writes back to the device before anything
 * names it (`dir_commit`). There the zeros are marked to go after the
 * device's `first` blocks, the pointers and inodes a free cleared, so that
 * a block freed and taken again is not overwritten on the device while an
 * old pointer there still names it; and since a block that names others
 * does not wait for an `after` one (bcache.h), the write is made now, not
 * left to the order of writing back. 0, or -errno.
 */
static int shared_block_new(struct ext2_mount *em, uint64_t off, const void *head, uint32_t len)
{
    int rc = ext2_write_bytes(em, off, 0, em->block_size, ext2_em_strict(em) ? 0 : WRITE_AFTER);

    if (rc == 0) {
        rc = ext2_write_bytes(em, off, head, len, WRITE_THROUGH | WRITE_KIND(READ_DATA));
    }
    if (rc == 0) {
        rc = dir_commit(em, off, em->block_size);
    }
    return rc;
}

/**
 * Make the stored inode of directory `d`, whose entries are about to
 * change, true of them before they do: when it is marked indexed, clear
 * `EXT2_INDEX_FL`, and when it has changed past its last store (its mark,
 * `ext2_node_dirty`), keep the change; then store the inode with
 * WRITE_THROUGH and, on a mount that writes back, write it to the device
 * (`inode_commit`). Every change of a directory's entries comes here
 * first, so a fenced directory (`d->fenced`), whose blocks cannot be
 * trusted, is -EIO here before any entry is written. 0, or -errno, with the
 * entries untouched.
 *
 * This driver keeps the linear entries only, which an index leaves readable
 * as they are: the index root lies in the slack of `..`, and each index
 * block is one empty entry spanning it. An index left set over changed
 * entries would be stale, so the flag goes, as Linux's ext2 drops it on
 * every change (fs/ext2/dir.c:468, 555 and 609 in Linux 6.12.115), and
 * e2fsck -D builds the index again. Storing the inode first means that no
 * stop leaves a changed directory marked indexed.
 *
 * A growth whose store failed (`dir_grow`) leaves the larger size, its new
 * pointer and `i_blocks` in memory with the mark, and the empty block it
 * covers would take the next name with no store of its own: a rename
 * retried after the failure would then put its new name in a block the
 * inode on the device does not reach and remove the old one, and a stop
 * before the next store of the inode would leave the file neither. So the
 * next change stores the marked inode and writes it first, as Linux writes
 * the directory's inode after each added entry (`ext2_handle_dirsync`,
 * fs/ext2/dir.c:440-447 and 557) before rename removes the old one
 * (namei.c:390). A store that succeeded and a write of it that failed set
 * the mark again, since the device lacks what the store put in the cache.
 *
 * A failed store, such as a failed read of the inode table before any write, or
 * a failed write of it, puts the flag back in memory and keeps the mark, so
 * that the next change stores the inode again before it touches an entry; a
 * flag left cleared in memory would let that change skip the store and write
 * over the index root in the slack of `..` while the disk still marks it
 * indexed. Linux's ext2 has no such store to fail: it clears i_flags in memory
 * as it changes the entries, and writeback copies the flags to the disk later
 * (fs/ext2/dir.c:468 and fs/ext2/inode.c:1567 in Linux 6.12.115). Should the
 * failed store have reached the disk, the entries are unchanged, and the next
 * store of the inode marks it indexed again, which is still true.
 */
static int dir_settle(struct ext2_mount *em, struct ext2_node *d)
{
    int indexed = (d->flags & EXT2_INDEX_FL) != 0;
    int rc;

    if (d->fenced) {
        return -EIO;
    }
    if (!indexed && !d->unstored) {
        return 0;
    }
    EXT2_NODE_SET(d, flags, d->flags & ~EXT2_INDEX_FL);
    rc = ext2_inode_store(em, d);
    if (rc == 0) {
        rc = inode_commit(em, d);
    }
    if (rc != 0) {
        EXT2_NODE_SET(d, flags, d->flags | (indexed ? EXT2_INDEX_FL : 0));
        return rc;
    }
    return 0;
}

/**
 * Add `name`, of `name_len` bytes, naming `ino` of file type `ftype`, to
 * directory `d` in a new block at its end, for `dir_add`, which found no room:
 * the block allocated and written with one empty entry spanning it
 * (`shared_block_new`), chained with `i_blocks` and covered by the size in one
 * store of the inode, and the entry written last. On a mount that writes back
 * the inode reaches the device before the entry is written (`dir_settle`), so
 * that a name the caller's next step depends on, such as a rename's new one
 * before the old goes, is reachable on the device when it is; a failure there
 * leaves an empty block and no name, so that a caller that frees the inode it
 * would have named frees one with no name, and leaves the directory marked
 * (`ext2_node_dirty`), so that the next change of its entries, which may put
 * a name in that block, stores and writes the inode first. A failure before the
 * size, of the block's write or of `ext2_map_alloc`'s store of a new indirect
 * block, leaves the pointer in memory past the size, where no entry is looked
 * for, with `i_blocks` raised under the mark its change set
 * (`ext2_node_blocks_add`), and the next growth maps the same block and writes
 * it again. Linux
 * writes the inode after the
 * entry (`ext2_handle_dirsync`), which puts both on the device before the next
 * step too. 0, -EFBIG for a directory that would pass 4 GiB less a block,
 * -ENOSPC, decided before any block is taken for the block and the pointer
 * blocks its index can need, or -errno.
 */
static int dir_grow(struct ext2_mount *em, struct ext2_node *d, const char *name, uint32_t name_len,
                    uint32_t ino, uint32_t ftype)
{
    uint32_t index = (uint32_t)(d->size >> em->block_shift);
    uint32_t b;
    uint64_t off, free;
    uint8_t head[8];
    int rc, fresh;

    if (d->size + em->block_size > 0xFFFFFFFFu) {
        return -EFBIG;
    }
    free = ext2_free_blocks(em);
    if (1 + ext2_pointer_levels(em, index) > free ||
        (!ext2_may_use_reserved() && free - (1 + ext2_pointer_levels(em, index)) < em->r_blocks)) {
        return -ENOSPC;
    }
    rc = ext2_map_alloc(em, d, index, &b, &fresh);
    if (rc != 0) {
        return rc;
    }
    off = (uint64_t)b * em->block_size;
    LE32_PUT(head, 0);
    LE16_PUT(head + 4, em->block_size);
    head[6] = 0;
    head[7] = 0;
    rc = shared_block_new(em, off, head, 8);
    if (rc != 0) {
        return rc;
    }
    ext2_node_size_set(d, d->size + em->block_size);
    ext2_stamp(d, T_MTIME | T_CTIME);
    rc = dir_settle(em, d);
    if (rc != 0) {
        return rc;
    }
    d->add_from = index << em->block_shift;
    rc = dirent_write(em, off, ino, em->block_size, name, name_len, ftype);
    return rc == 0 ? dir_commit(em, off, dirent_len(name_len)) : rc;
}

/**
 * Add `name` naming `ino`, of file type `ftype`, to directory `d`: in the
 * first empty record with room, whole, or the first entry with room behind
 * its name, split; else in a new block (`dir_grow`). The entry itself is
 * the last write, and on a mount that writes back reaches the device before
 * the call returns (`dir_commit`). 0,
 * -ENOENT for a removed directory, -ENAMETOOLONG, -EFBIG for a directory
 * that would pass 4 GiB less a block, -ENOSPC, decided before any block is
 * taken for the block and the pointer blocks its index can need, or -errno.
 *
 * The search for room starts at the block the last entry went into and
 * wraps around to it, so a directory that grows at its end is not read
 * whole for each name added to it; a block before that one still takes a
 * name before the directory grows.
 */
static int dir_add(struct ext2_mount *em, struct ext2_node *d, const char *name, uint32_t ino,
                   uint32_t ftype)
{
    uint32_t name_len = (uint32_t)k_strlen(name);
    uint32_t need = dirent_len(name_len);
    uint32_t start = d->add_from < d->size ? d->add_from : 0;
    uint32_t cursor = start, limit = (uint32_t)d->size;
    struct ext2_scan sc;
    struct ext2_window w;
    uint8_t entry[8 + EXT2_NAME_MAX + 4];
    int rc;

    if (d->unlinked) {
        return -ENOENT;
    }
    if (name_len == 0 || name_len > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    rc = dir_settle(em, d);
    if (rc != 0) {
        return rc;
    }
    dir_changed(d);
    window_open(em, &w);
    for (;;) {
        uint32_t here = cursor;
        uint32_t used, room;
        uint64_t off;
        int got = here < limit ? dir_scan_in(em, d, &w, &cursor, &sc) : 0;

        if (got < 0) {
            window_close(&w);
            return got;
        }
        if (got == 0 && start != 0) {
            cursor = 0;
            limit = start;
            start = 0;
            continue;
        }
        if (got == 0) {
            break;
        }
        used = sc.ino == 0 ? 0 : dirent_len(sc.name_len);
        room = sc.rec_len - used;
        if (room < need) {
            continue;
        }
        window_close(&w);
        rc = dir_block_at(em, d, here, &off);
        if (rc != 0) {
            return rc;
        }
        d->add_from = here & ~(em->block_size - 1u);
        if (used == 0) {
            rc = dirent_write(em, off, ino, sc.rec_len, name, name_len, ftype);
        } else {
            used = dirent_encode(entry, ino, room, name, name_len,
                                 (em->incompat & EXT2_INCOMPAT_FILETYPE) ? ftype : 0);
            rc = dirent_split(em, off, &sc, entry, used);
        }
        return rc == 0 ? dir_commit(em, off, sc.rec_len) : rc;
    }
    window_close(&w);
    return dir_grow(em, d, name, name_len, ino, ftype);
}

/** Remove the entry `sc` at byte `at` of `d`, whose predecessor in its block
 * is at `prev_at`: the first in its block has its inode cleared, and any
 * other is merged into its predecessor's record, with WRITE_THROUGH. 0, or
 * -errno. */
static int dir_remove_at(struct ext2_mount *em, struct ext2_node *d, uint32_t at, uint32_t prev_at,
                         const struct ext2_scan *sc)
{
    uint64_t off;
    int rc = dir_settle(em, d);

    if (rc != 0) {
        return rc;
    }
    dir_changed(d);
    if (at % em->block_size == 0) {
        uint8_t zero[4] = {0, 0, 0, 0};

        rc = dir_block_at(em, d, at, &off);
        if (rc == 0) {
            rc = ext2_write_bytes(em, off, zero, 4, WRITE_THROUGH | WRITE_KIND(READ_DATA));
        }
        return rc == 0 ? dir_commit(em, off, 4) : rc;
    } else {
        struct ext2_scan ps;
        uint32_t cursor = prev_at;
        uint8_t raw[2];
        int got = dir_scan(em, d, &cursor, &ps);

        if (got <= 0) {
            return got < 0 ? got : dir_damaged(em, d->ino, "an entry's predecessor is gone");
        }
        rc = dir_block_at(em, d, prev_at, &off);
        if (rc != 0) {
            return rc;
        }
        LE16_PUT(raw, ps.rec_len + sc->rec_len);
        rc = ext2_write_bytes(em, off + 4, raw, 2, WRITE_THROUGH | WRITE_KIND(READ_DATA));
        return rc == 0 ? dir_commit(em, off + 4, 2) : rc;
    }
}

/** Whether directory `d` has nothing but `.` and `..`, into `*empty`; 0, or
 * -EIO. */
static int dir_is_empty(struct ext2_mount *em, const struct ext2_node *d, int *empty)
{
    uint32_t cursor = 0;
    struct ext2_scan sc;

    *empty = 1;
    for (;;) {
        int got = dir_scan(em, d, &cursor, &sc);

        if (got < 0) {
            return got;
        }
        if (got == 0) {
            return 0;
        }
        if (sc.ino != 0 && !names_equal(sc.name, ".") && !names_equal(sc.name, "..")) {
            *empty = 0;
            return 0;
        }
    }
}

/** Make the `..` entry of directory `d` name `parent`, with WRITE_THROUGH; 0,
 * -EIO for a directory without one, which is damage (`dir_damaged`), or
 * -errno. */
static int dir_set_dotdot(struct ext2_mount *em, struct ext2_node *d, uint32_t parent)
{
    uint32_t cursor = 0;
    struct ext2_scan sc;
    uint8_t raw[4];
    uint64_t off;
    int rc;

    for (;;) {
        uint32_t here = cursor;
        int got = dir_scan(em, d, &cursor, &sc);

        if (got <= 0) {
            return got < 0 ? got : dir_damaged(em, d->ino, "it has no '..'");
        }
        if (sc.ino != 0 && names_equal(sc.name, "..")) {
            rc = dir_block_at(em, d, here, &off);
            if (rc == 0) {
                rc = dir_settle(em, d);
            }
            if (rc != 0) {
                return rc;
            }
            dir_changed(d);
            LE32_PUT(raw, parent);
            rc = ext2_write_bytes(em, off, raw, 4, WRITE_THROUGH | WRITE_KIND(READ_DATA));
            return rc == 0 ? dir_commit(em, off, 4) : rc;
        }
    }
}

/**
 * Whether directory inode `ino` is `ancestor` or below it, into `*below`,
 * by the `..` entries from `ino` up to the root: 0, -ELOOP past PATH_DEPTH
 * levels, -EIO for a directory without `..` or for a freed inode one names,
 * which are damage (`dir_damaged`, `ext2_stale`), or -errno.
 *
 * The caller holds the mount's rename lock, under which no directory
 * changes its parent, and may hold the locks of directories on the way;
 * so the walk reads copies of the inodes and takes no node lock, since one
 * of an ancestor would come after its descendant's, and no big lock. Each
 * read of an inode or of an entry's head copies from the block's buffer
 * while it holds it, or from the batch's image under the batch's spinlock
 * (`ext2_read_bytes`), as each write through the cache copies into both
 * while it holds the buffer (`ext2_write_owned`); so a `..` read while
 * another call adds or removes a name beside it, under that directory's
 * lock alone, is whole, its record length before the change or after.
 */
static int is_below(struct ext2_mount *em, uint32_t ino, uint32_t ancestor, int *below)
{
    int depth;

    *below = 0;
    for (depth = 0; depth < PATH_DEPTH; depth++) {
        struct ext2_node cur;
        struct ext2_scan sc;
        uint32_t cursor = 0;
        int rc;

        if (ino == ancestor) {
            *below = 1;
            return 0;
        }
        if (ino == EXT2_ROOT_INO) {
            return 0;
        }
        rc = ext2_inode_load(em, ino, &cur);
        if (rc != 0) {
            return rc == -ESTALE ? ext2_stale(em, ino) : rc;
        }
        for (;;) {
            int got = dir_scan(em, &cur, &cursor, &sc);

            if (got < 0) {
                return got;
            }
            if (got == 0) {
                return dir_damaged(em, ino, "it has no '..'");
            }
            if (sc.ino != 0 && names_equal(sc.name, "..")) {
                break;
            }
        }
        ino = sc.ino;
    }
    return -ELOOP;
}

/**
 * A new inode of mode `mode` in the directory `dir`, allocated, written
 * initialised and held once in a new node, into `*out`; the caller names
 * it. Its owner is the caller's effective uid, and its group the caller's
 * effective gid, or the directory's when the directory is setgid, which
 * then passes the bit on to a new directory, as Linux's `inode_init_owner`
 * does. Its link count is 2 for a directory and 1 otherwise.
 *
 * With `store` the inode is written whole over zeros; without, its slot is
 * written zeroed, for a caller that stores the inode once it has the rest,
 * and the node keeps its mark (`ext2_node_dirty`) until then.
 * Either way the slot's old bytes are gone, since a freed inode's slot
 * keeps what it had and the encoder writes only the fields this driver
 * keeps. The node is made before a group's lock, whose holder should not
 * wait on the heap, and is published N_LOADING under it, so that no gap
 * lets another caller see the number with no node; it goes N_LIVE once
 * written. The parent's group's inode bitmap and descriptor are fetched
 * before the lock, and a search that meets another group's that the cache
 * lacks, or a block another process holds, lets the lock go, fetches it or
 * waits for it and starts again, PLAN_TRIES times before it reads under
 * the lock, so that the holder waits on the disk only for a block evicted
 * again.
 * The caller locks the new inode at subclass 4, below the published nodes
 * the VFS holds, until it has a directory entry. The big lock is taken for
 * the whole of it (`inode_new`), since the allocator looks the number up in
 * the node table and publishes the node there.
 * 0, -ENOSPC, -ENOMEM, or -errno; a failure frees the inode.
 */
static int inode_make(struct ext2_mount *em, const struct ext2_node *dir, uint32_t mode, int store,
                      struct ext2_node **out)
{
    uint32_t parent_ino = dir->ino;
    int is_dir = (mode & S_IFMT_) == S_IFDIR_;
    struct ext2_node *n = ext2_node_make();
    uint32_t missed;
    int tries, rc;

    if (!n) {
        klog(KLOG_WARN, "ext2", "node: no memory");
        return -ENOMEM;
    }
    ext2_block_fetch(em, (uint64_t)ext2_le32(em->gd + (parent_ino - 1) / em->ipg * 32 + 4) *
                             em->block_size);
    ext2_gd_fetch(em, (parent_ino - 1) / em->ipg);
    for (tries = 0;; tries++) {
        ext2_plan_begin(em, tries);
        rc = ext2_inode_alloc(em, parent_ino, is_dir, n);
        missed = ext2_plan_end(em);
        if (rc != -EAGAIN) {
            break;
        }
        ext2_volume_fetch(em, missed);
    }
    if (rc != 0) {
        if (n->ino != 0) {
            ext2_node_settle(em, n, rc);
        } else {
            kfree(n);
        }
        return rc;
    }
    EXT2_NODE_SET(n, uid, current_proc ? proc_fsuid(current_proc) : 0);
    EXT2_NODE_SET(n, gid, current_proc ? proc_fsgid(current_proc) : 0);
    if (dir->mode & S_ISGID_) {
        EXT2_NODE_SET(n, gid, dir->gid);
        if ((mode & S_IFMT_) == S_IFDIR_) {
            mode |= S_ISGID_;
        }
    }
    EXT2_NODE_SET(n, mode, mode);
    ext2_node_nlink_set(n, (mode & S_IFMT_) == S_IFDIR_ ? 2 : 1);
    EXT2_NODE_SET(n, extra_isize, em->new_extra);
    ext2_stamp(n, T_ATIME | T_MTIME | T_CTIME);
    if (ext2_extra_has(n, EXT2_CRTIME + 4)) {
        EXT2_NODE_SET(n, crtime,
                      ext2_time_fit(n, EXT2_CRTIME_EXTRA + 4, n->ctime.sec, n->ctime.nsec));
    }
    EXT2_NODE_SET(n, generation, ext2_generation_next(em));
    rc = store ? ext2_inode_write(em, n, 1, WRITE_THROUGH)
               : ext2_write_bytes(em, ext2_inode_offset(em, n->ino), 0, em->inode_size,
                                  WRITE_THROUGH | WRITE_KIND(READ_SLOT));
    if (rc != 0) {
        ext2_inode_bit_free(em, n->ino, is_dir);
        ext2_node_settle(em, n, rc);
        return rc;
    }
    ext2_node_settle(em, n, 0);
    *out = n;
    return 0;
}

/** `inode_make` under the big lock, taken for an operation on names, which
 * runs without it. */
static int inode_new(struct ext2_mount *em, const struct ext2_node *dir, uint32_t mode, int store,
                     struct ext2_node **out)
{
    int big = ext2_big_enter();
    int rc = inode_make(em, dir, mode, store, out);

    ext2_big_exit(big);
    return rc;
}

/**
 * The node of inode `ino`, which an entry of a directory whose lock the
 * caller holds names, held and locked, into `*out`: the one node every
 * holder of the inode shares (`ext2_node_for`), never a copy, so that a change
 * made here is the one an open file sees. The VFS locks the node of the
 * name in unlink, rmdir and rename before it calls the type, and this takes
 * that lock again by depth. 0, or the errors of `ext2_node_for`, a freed
 * inode reported as damage (`ext2_stale`), since the caller holds the lock
 * of the directory that names it, or -EIO with nothing held for a fenced
 * node (`c->fenced`), whose entry stays and whose inode is neither stored
 * nor freed, for e2fsck.
 */
static int child_take(struct ext2_mount *em, uint32_t ino, struct ext2_node **out)
{
    int rc;
    struct ext2_node *c = ext2_node_for(em, ino, &rc);

    if (!c) {
        return rc == -ESTALE ? ext2_stale(em, ino) : rc;
    }
    ext2_node_lock(c);
    if (c->fenced) {
        ext2_node_unlock(c);
        ext2_put(em->mnt, c);
        return -EIO;
    }
    *out = c;
    return 0;
}

/** Give back the lock and the hold of a node `child_take` gave. */
static void child_give(struct ext2_mount *em, struct ext2_node *c)
{
    ext2_node_unlock(c);
    ext2_put(em->mnt, c);
}

/**
 * Give up the new inode `n` an operation on names made and could not name,
 * before any entry names it: its link count 0 and `unlinked` set in memory,
 * which no store needs, since nothing names it, and its lock and the
 * caller's hold, the only one, given back (`child_give`). Its last put then
 * frees it (`node_free`), at once or, under the caller's file locks, at the
 * end of the operation, as Linux's `out_fail` drops the count and lets
 * `iput` free the inode (fs/ext2/namei.c:157-266). A free that fails is
 * logged there and leaves an orphan for e2fsck; the reader reported the
 * fault it met, and a failed write is recorded as one.
 */
static void inode_abandon(struct ext2_mount *em, struct ext2_node *n)
{
    ext2_node_nlink_set(n, 0);
    n->unlinked = 1;
    child_give(em, n);
}

/**
 * Mark node `c`, whose lock the caller holds and whose link count is 0,
 * as having no name, with its change time set: freed at once when the
 * caller's is its only hold, since nothing else can reach it, and
 * otherwise stored with its count of 0, for e2fsck to read it as deleted
 * after a crash, and freed at its last put. The count of holds is read
 * under the big lock, with the free begun in the same step, since only
 * `ext2_node_for` under it gives a new hold on a node nothing else holds.
 * 0, or -errno.
 */
static int child_unlinked(struct ext2_mount *em, struct ext2_node *c)
{
    int big = ext2_big_enter();
    int rc;

    ext2_stamp(c, T_CTIME);
    c->unlinked = 1;
    rc = c->refs == 1 ? ext2_release_inode(em, c) : ext2_inode_store(em, c);
    ext2_big_exit(big);
    return rc;
}

/**
 * Lower the link count of node `c`, whose lock the caller holds and one of
 * whose names a change of an entry on the device has just removed: the
 * inode freed, or stored with its count of 0, when no name is left
 * (`child_unlinked`), and otherwise stored with its change time. The
 * operation has happened once the entry is gone, so nothing here fails it,
 * as Linux's `ext2_unlink` answers for the entry alone (fs/ext2/namei.c:
 * 284-287): a store that fails keeps the node's mark, which its next store
 * settles, at its last put at the latest, and a free that fails is made
 * again at the last put, which logs one that fails there. Either failure
 * was reported where it arose or is a device's.
 */
static void child_drop_link(struct ext2_mount *em, struct ext2_node *c)
{
    if (c->nlink > 0) {
        ext2_node_nlink_set(c, c->nlink - 1);
    }
    if (c->nlink == 0) {
        (void)child_unlinked(em, c);
    } else {
        ext2_stamp(c, T_CTIME);
        (void)ext2_inode_store(em, c);
    }
}

/**
 * Lower the link count of directory `d`, one of whose subdirectories a
 * change of an entry on the device has just removed or moved away, when
 * `lose` is set and the count is above 2, and store `d` with its times. The
 * entry is gone, so a failed store fails nothing: it keeps `d`'s mark, which
 * its next store settles, at its last put at the latest; the failure was
 * reported where it arose or is a device's.
 */
static void dir_lose_parent(struct ext2_mount *em, struct ext2_node *d, int lose)
{
    if (lose && d->nlink > 2) {
        ext2_node_nlink_set(d, d->nlink - 1);
    }
    ext2_stamp(d, T_MTIME | T_CTIME);
    (void)ext2_inode_store(em, d);
}

/**
 * The `create` of `vfs_ops`: a new regular file `name` in `dir`, the root for
 * NULL, with the permission bits of `mode`: the inode allocated and written
 * whole, then the entry that names it, then the directory's times into the
 * cache. A failure to add the entry frees the inode (`inode_abandon`); a
 * failure to store the times is not the call's, since the entry is there,
 * and is not reported again: a failed read of the table block was reported
 * as it was met (READ_SLOT).
 * The batch is drained first. The caller holds the directory's lock, and
 * the new node's is held until it is named. 0 with the node held, -ENOENT
 * for a removed directory, -EEXIST, -ENAMETOOLONG, -EROFS, -ENOSPC,
 * -ENOMEM, or -errno.
 */
int ext2_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    struct ext2_scan sc;
    uint32_t at, prev;
    int rc;

    rc = ext2_assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    ext2_assert_node_locked(d);
    if (d->unlinked) {
        return -ENOENT;
    }
    if (k_strlen(name) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    rc = inode_new(em, d, S_IFREG_ | (mode & 07777u), 1, &n);
    if (rc != 0) {
        return rc;
    }
    sleep_lock_subclass(4);
    ext2_node_lock(n);
    sleep_lock_subclass(0);
    rc = dir_add(em, d, name, n->ino, 1);
    if (rc != 0) {
        inode_abandon(em, n);
        return rc;
    }
    ext2_stamp(d, T_MTIME | T_CTIME);
    (void)ext2_inode_times_later(em, d);
    ext2_node_unlock(n);
    *node = n;
    return 0;
}

/**
 * The `mknod` of `vfs_ops`: a FIFO, a socket or a device node `name` in
 * `dir`, in the order `create` keeps: the inode's bit, then the inode written
 * whole, then the entry that names it, then the directory's times. A
 * device's number is where Linux keeps it: the old encoding, a byte each of
 * major and minor, in the first pointer word when both fit, and the new
 * encoding in the second word otherwise, with the first zero. The caller
 * holds the directory's lock. 0 with the node held, -EINVAL for another
 * kind, or the errors of `ext2_create`.
 */
int ext2_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, uint32_t rdev,
               void **node)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    struct ext2_scan sc;
    uint32_t type = mode & S_IFMT_;
    uint32_t at, prev;
    int rc;

    if (type != S_IFIFO_ && type != S_IFSOCK_ && type != S_IFCHR_ && type != S_IFBLK_) {
        return -EINVAL;
    }
    rc = ext2_assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    ext2_assert_node_locked(d);
    if (d->unlinked) {
        return -ENOENT;
    }
    if (k_strlen(name) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    rc = inode_new(em, d, type | (mode & 07777u), 0, &n);
    if (rc != 0) {
        return rc;
    }
    sleep_lock_subclass(4);
    ext2_node_lock(n);
    sleep_lock_subclass(0);
    if (type == S_IFCHR_ || type == S_IFBLK_) {
        uint32_t major = (rdev >> 8) & 0xFFFu;
        uint32_t minor = (rdev & 0xFFu) | ((rdev >> 12) & 0xFFF00u);

        if (major < 256 && minor < 256) {
            EXT2_NODE_SET(n, block[0], (major << 8) | minor);
        } else {
            EXT2_NODE_SET(n, block[1], (minor & 0xFFu) | (major << 8) | ((minor & 0xFFF00u) << 12));
        }
    }
    rc = ext2_inode_store(em, n);
    if (rc == 0) {
        rc = dir_add(em, d, name, n->ino, ext2_ftype_of(n->mode));
    }
    if (rc != 0) {
        inode_abandon(em, n);
        return rc;
    }
    ext2_stamp(d, T_MTIME | T_CTIME);
    (void)ext2_inode_times_later(em, d);
    ext2_node_unlock(n);
    *node = n;
    return 0;
}

/**
 * The `unlink` of `vfs_ops`: the entry `name` in `dir` removed, then the
 * directory's times, then the inode's link count lowered, on the node every
 * holder shares, under its lock (`child_take`). The inode goes with its last
 * name when nothing else holds it, and at the last put otherwise, its count
 * of zero stored meanwhile, so that after a crash e2fsck and the check read
 * it as deleted (`child_unlinked`). Once the entry is gone the call has
 * happened, and a failed store of the times or of the count fails nothing
 * (`child_drop_link`). The batch is drained first. The caller holds the
 * directory's lock. 0, -ENOENT, -EISDIR, -EROFS, or -errno.
 */
int ext2_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n = 0;
    struct ext2_scan sc;
    uint32_t at, prev;
    int rc;

    rc = ext2_assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    ext2_assert_node_locked(d);
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -ENOENT;
    }
    rc = child_take(em, sc.ino, &n);
    if (rc != 0) {
        return rc;
    }
    rc = (n->mode & S_IFMT_) == S_IFDIR_ ? -EISDIR : dir_remove_at(em, d, at, prev, &sc);
    if (rc == 0) {
        ext2_stamp(d, T_MTIME | T_CTIME);
        (void)ext2_inode_times_later(em, d);
        child_drop_link(em, n);
    }
    child_give(em, n);
    return rc;
}

/**
 * The `mkdir` of `vfs_ops`: a new directory `name` in `dir`, with the
 * permission bits of `mode`. Its block is written with `.` and `..`, then its
 * inode with a count of two, then the parent's raised count, and the parent's
 * entry last; a stop before the inode leaves a set bit with nothing behind it.
 * On a mount that writes back the block and the inode reach the device before
 * the parent's entry names them (`shared_block_new`, `inode_commit`), as
 * Linux's `ext2_make_empty` writes both (fs/ext2/dir.c:650 in Linux 6.12.115),
 * and so does the parent's inode with its raised count, which Linux writes
 * after the entry (`ext2_mkdir` raises it before `ext2_add_link`, whose
 * `ext2_handle_dirsync` ends in `sync_inode_metadata`): before it, a stop
 * leaves the count above the names rather than below them.
 * A failure frees the new inode (`inode_abandon`), and lowers the parent's
 * count again in memory when the entry was not written, which the parent's
 * mark has its next store write. The batch is drained first. The caller
 * holds the parent's lock, and the new directory's is held until it is
 * named. 0, -ENOENT for a removed parent, -EEXIST, -EMLINK at 65000 links,
 * -ENAMETOOLONG, -EROFS, -ENOSPC, -ENOMEM, or -errno.
 */
int ext2_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    struct ext2_scan sc;
    uint32_t at, prev, b;
    uint8_t head[24];
    int rc, fresh;

    rc = ext2_assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    ext2_assert_node_locked(d);
    if (d->unlinked) {
        return -ENOENT;
    }
    if (k_strlen(name) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    if (d->nlink >= 65000) {
        return -EMLINK;
    }
    rc = inode_new(em, d, S_IFDIR_ | (mode & 07777u), 0, &n);
    if (rc != 0) {
        return rc;
    }
    sleep_lock_subclass(4);
    ext2_node_lock(n);
    sleep_lock_subclass(0);
    rc = ext2_map_alloc(em, n, 0, &b, &fresh);
    if (rc == 0) {
        dir_changed(n);
        k_memset(head, 0, sizeof(head));
        LE32_PUT(head, n->ino);
        LE16_PUT(head + 4, 12);
        head[6] = 1;
        head[7] = (em->incompat & EXT2_INCOMPAT_FILETYPE) ? 2 : 0;
        head[8] = '.';
        LE32_PUT(head + 12, d->ino);
        LE16_PUT(head + 16, em->block_size - 12);
        head[18] = 2;
        head[19] = head[7];
        head[20] = '.';
        head[21] = '.';
        rc = shared_block_new(em, (uint64_t)b * em->block_size, head, 24);
    }
    if (rc == 0) {
        ext2_node_size_set(n, em->block_size);
        rc = ext2_inode_store(em, n);
    }
    if (rc == 0) {
        rc = inode_commit(em, n);
    }
    if (rc == 0) {
        ext2_node_nlink_set(d, d->nlink + 1);
        ext2_stamp(d, T_MTIME | T_CTIME);
        rc = ext2_inode_store(em, d);
        if (rc == 0) {
            rc = inode_commit(em, d);
        }
        if (rc == 0) {
            rc = dir_add(em, d, name, n->ino, 2);
        }
        if (rc != 0) {
            ext2_node_nlink_set(d, d->nlink - 1);
        }
    }
    if (rc != 0) {
        inode_abandon(em, n);
        return rc;
    }
    ext2_node_unlock(n);
    ext2_node_unhold(em, n);
    return 0;
}

/**
 * The `rmdir` of `vfs_ops`: the empty directory `name` in `dir` removed: the
 * entry, then the parent's count lowered, then the inode freed. The
 * directory's own lock is held from the check that it is empty until it is
 * marked as having no name, with a link count of 0, so that no entry is
 * added to it meanwhile (`dir_add`) and a mount on it is refused after
 * (`vfs_cover_lock`). One that a working directory or an open description
 * holds keeps its inode and its blocks until its last put, as an open file
 * does; nothing can be added to it, and a listing of it is -ENOENT, as on
 * Linux. Once the entry is gone the call has happened, and a failed store
 * of the parent or the free of the inode fails nothing (`dir_lose_parent`,
 * `child_unlinked`). The batch is drained first. The caller holds the
 * parent's lock. 0, -ENOENT, -ENOTDIR, -ENOTEMPTY, -EROFS, or -errno.
 */
int ext2_rmdir(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *c = 0;
    struct ext2_scan sc;
    uint32_t at, prev;
    int rc, empty = 0;

    rc = ext2_assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    ext2_assert_node_locked(d);
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 0) {
        return -ENOENT;
    }
    rc = child_take(em, sc.ino, &c);
    if (rc != 0) {
        return rc;
    }
    rc = (c->mode & S_IFMT_) != S_IFDIR_ ? -ENOTDIR : dir_is_empty(em, c, &empty);
    if (rc == 0 && !empty) {
        rc = -ENOTEMPTY;
    }
    if (rc == 0) {
        rc = dir_remove_at(em, d, at, prev, &sc);
    }
    if (rc == 0) {
        ext2_node_nlink_set(c, 0);
        c->unlinked = 1;
        dir_lose_parent(em, d, 1);
        (void)child_unlinked(em, c);
    }
    child_give(em, c);
    return rc;
}

/**
 * The name `name` in directory `d` for the file `n`, for `ext2_link`, whose
 * caller holds both locks: the count raised and stored, then the name
 * written, then the directory's times, whose failed store, once the name is
 * there, fails nothing and keeps the directory's mark. 0, -ENOENT for a file
 * with no name left, -EMLINK, -EEXIST, or -errno.
 */
static int link_add(struct ext2_mount *em, struct ext2_node *d, const char *name,
                    struct ext2_node *n)
{
    struct ext2_scan sc;
    uint32_t at, prev;
    int rc;

    if (n->unlinked || n->nlink == 0) {
        return -ENOENT;
    }
    if (n->nlink >= 65000) {
        return -EMLINK;
    }
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    ext2_node_nlink_set(n, n->nlink + 1);
    ext2_stamp(n, T_CTIME);
    rc = ext2_inode_store(em, n);
    if (rc != 0) {
        ext2_node_nlink_set(n, n->nlink - 1);
        return rc;
    }
    rc = dir_add(em, d, name, n->ino, ext2_ftype_of(n->mode));
    if (rc != 0) {
        ext2_node_nlink_set(n, n->nlink - 1);
        return rc;
    }
    ext2_stamp(d, T_MTIME | T_CTIME);
    (void)ext2_inode_times_later(em, d);
    return 0;
}

/**
 * The `link` of `vfs_ops`: a new name `name` in `dir` for the file `node`:
 * the count raised first, then the name written, so that a stop between
 * leaves a count one too high, which is a count repair; then the
 * directory's times. A failure to write the name lowers the count again in
 * memory, which the file's mark has its next store write.
 * The batch is drained first. The caller holds the directory's lock and
 * the file's, which this takes again. 0, -EPERM for a directory, -ENOENT
 * for a removed directory or a file with no name left, -EMLINK at 65000
 * links, -ENAMETOOLONG, -EEXIST, -EROFS, -ENOSPC, or -errno.
 */
int ext2_link(struct vfs_mount *mnt, void *dir, const char *name, void *node)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n = node;
    int rc;

    rc = ext2_assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    ext2_assert_node_locked(d);
    if ((n->mode & S_IFMT_) == S_IFDIR_) {
        return -EPERM;
    }
    if (d->unlinked) {
        return -ENOENT;
    }
    if (k_strlen(name) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    ext2_node_lock(n);
    rc = link_add(em, d, name, n);
    ext2_node_unlock(n);
    return rc;
}

/**
 * The `symlink` of `vfs_ops`: a link `name` in `dir` to `target`. A target
 * shorter than 60 bytes is kept in the inode's pointer words, and a longer one
 * in a block of its own, written first (`shared_block_new`); then the inode,
 * then the entry, then the directory's times. A failure to add the entry frees
 * the inode; a failure to store the times is not the call's, since the
 * entry, on the device on a mount that writes back, names the inode, as in
 * `ext2_create`. The batch is drained first. The caller holds the
 * directory's lock, and the link's is held until it is named. 0, -ENOENT for a
 * removed directory, -ENAMETOOLONG for a name past 255 bytes or for a target
 * that is empty or of a block or more, -EEXIST, -EROFS, -ENOSPC, -ENOMEM, or
 * -errno.
 */
int ext2_symlink(struct vfs_mount *mnt, void *dir, const char *name, const char *target)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    struct ext2_scan sc;
    uint32_t at, prev;
    uint32_t tlen = (uint32_t)k_strlen(target);
    int rc;

    rc = ext2_assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    ext2_assert_node_locked(d);
    if (d->unlinked) {
        return -ENOENT;
    }
    if (k_strlen(name) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    if (tlen == 0 || tlen >= em->block_size) {
        return -ENAMETOOLONG;
    }
    rc = dir_find(em, d, name, &at, &prev, &sc);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        return -EEXIST;
    }
    rc = inode_new(em, d, S_IFLNK_ | 0777u, 0, &n);
    if (rc != 0) {
        return rc;
    }
    sleep_lock_subclass(4);
    ext2_node_lock(n);
    sleep_lock_subclass(0);
    if (tlen < EXT2_FAST_LINK) {
        uint32_t i;

        for (i = 0; i < tlen; i++) {
            EXT2_NODE_SET(n, block[i / 4],
                          n->block[i / 4] | (uint32_t)(uint8_t)target[i] << ((i % 4) * 8));
        }
        n->fast_target = 1;
    } else {
        uint32_t b;
        int fresh;

        rc = ext2_map_alloc(em, n, 0, &b, &fresh);
        if (rc == 0) {
            rc = shared_block_new(em, (uint64_t)b * em->block_size, target, tlen);
        }
    }
    if (rc == 0) {
        ext2_node_size_set(n, tlen);
        rc = ext2_inode_store(em, n);
    }
    if (rc == 0) {
        rc = dir_add(em, d, name, n->ino, 7);
    }
    if (rc != 0) {
        inode_abandon(em, n);
        return rc;
    }
    ext2_stamp(d, T_MTIME | T_CTIME);
    (void)ext2_inode_times_later(em, d);
    ext2_node_unlock(n);
    ext2_node_unhold(em, n);
    return 0;
}

/**
 * Whether the rename of node `n` from directory `od` over `victim` in
 * directory `nd`, `victim` NULL for a new name, may go on, decided before
 * either node's lock is taken: 0; -EISDIR or -ENOTDIR for a directory and
 * a file; -EINVAL for a directory moved below itself; -ENOTEMPTY for a
 * victim directory above `od`, which holds it; or the errors of `is_below`.
 *
 * The VFS leaves a node unlocked in the last two cases, since a directory
 * above the other one would be locked after its descendant, so they are
 * found here first, by walks up `..` under the mount's rename lock. A
 * file's or a directory's type does not change while it is held.
 */
static int rename_check(struct ext2_mount *em, struct ext2_node *od, struct ext2_node *nd,
                        struct ext2_node *n, struct ext2_node *victim)
{
    int is_dir = (n->mode & S_IFMT_) == S_IFDIR_;
    int below, rc;

    if (victim) {
        int victim_dir = (victim->mode & S_IFMT_) == S_IFDIR_;

        if (victim_dir && !is_dir) {
            return -EISDIR;
        }
        if (!victim_dir && is_dir) {
            return -ENOTDIR;
        }
    }
    if (!is_dir || od == nd) {
        return 0;
    }
    rc = is_below(em, nd->ino, n->ino, &below);
    if (rc != 0 || below) {
        return rc != 0 ? rc : -EINVAL;
    }
    if (victim) {
        rc = is_below(em, od->ino, victim->ino, &below);
        if (rc != 0 || below) {
            return rc != 0 ? rc : -ENOTEMPTY;
        }
    }
    return 0;
}

/* The two names of one `ext2_rename`, each with its directory. */
struct ext2_rename_names {
    struct ext2_mount *em;
    struct ext2_node *od;
    const char *oldname;
    struct ext2_node *nd;
    const char *newname;
};

/**
 * The rename of directory `n` from `r->oldname` in `r->od` to `r->newname`
 * in `r->nd`, over the empty directory `victim` at `nsc`, `nat` and `nprev`
 * when it is not NULL, for `ext2_rename`, whose caller holds every lock: the
 * victim removed as rmdir removes it, the new parent's count lowered for the
 * victim's `..`, then the new entry, then `..` rewritten and the new parent's
 * count raised when the parents differ, then the old entry removed and the old
 * parent's count lowered when the parents differ. Within one parent the count
 * falls by one, for the victim; between two, the new parent's is unchanged
 * with a victim and the old parent's falls by one, as Linux's `ext2_rename`
 * leaves `new_dir` with a target and drops `old_dir`
 * (fs/ext2/namei.c:354-396). A failed store of a count or of times after an
 * entry change is on the device fails nothing: the node keeps its mark, which
 * its next store settles. 0, -ENOENT, -EMLINK when the parents differ, there
 * is no victim and the new parent's count would pass 65000, as Linux tests it
 * only when the new name is no directory (fs/namei.c:5090-5093), or -errno.
 */
static int rename_dir(const struct ext2_rename_names *r, struct ext2_node *n,
                      struct ext2_node *victim, const struct ext2_scan *nsc, uint32_t nat,
                      uint32_t nprev)
{
    struct ext2_mount *em = r->em;
    struct ext2_node *od = r->od;
    struct ext2_node *nd = r->nd;
    struct ext2_scan osc;
    uint32_t oat, oprev;
    int rc;

    if (od != nd && !victim && nd->nlink >= 65000) {
        return -EMLINK;
    }
    if (victim) {
        rc = dir_remove_at(em, nd, nat, nprev, nsc);
        if (rc != 0) {
            return rc;
        }
        ext2_node_nlink_set(victim, 0);
        victim->unlinked = 1;
        dir_lose_parent(em, nd, 1);
        (void)child_unlinked(em, victim);
    }
    rc = dir_add(em, nd, r->newname, n->ino, 2);
    if (rc != 0) {
        return rc;
    }
    if (od != nd) {
        rc = dir_set_dotdot(em, n, nd->ino);
        if (rc != 0) {
            return rc;
        }
        ext2_node_nlink_set(nd, nd->nlink + 1);
        ext2_stamp(nd, T_MTIME | T_CTIME);
        (void)ext2_inode_store(em, nd);
    }
    rc = dir_find(em, od, r->oldname, &oat, &oprev, &osc);
    if (rc <= 0) {
        return rc < 0 ? rc : -ENOENT;
    }
    rc = dir_remove_at(em, od, oat, oprev, &osc);
    if (rc == 0) {
        dir_lose_parent(em, od, od != nd);
    }
    return rc;
}

/**
 * The rename of file `n` from `r->oldname` in `r->od` to `r->newname` in
 * `r->nd`, over the file `victim` whose entry is `nsc` at `nat` when it is
 * not NULL, for `ext2_rename`, whose caller holds every lock: the count
 * raised, the new entry written or the victim's entry pointed at `n`, the
 * victim's count lowered as unlink lowers it, the old entry removed and the
 * count lowered again. A failed store of a count or of times after an entry
 * change is on the device fails nothing: the node keeps its mark, which its
 * next store settles, so a rename whose entries both changed succeeds. 0,
 * -ENOENT, or -errno.
 *
 * The victim's entry takes `n`'s inode number and file type, 0 on a volume
 * without the filetype feature, in one write of its 8-byte header, as
 * Linux's `ext2_set_link` sets both and commits them as one chunk
 * (fs/ext2/dir.c:463-465 in Linux 6.12.115), so that no stop on a strict
 * mount leaves the entry naming `n` with the victim's type, which e2fsck
 * calls a fault when a link is renamed over a regular file.
 */
static int rename_file(const struct ext2_rename_names *r, struct ext2_node *n,
                       struct ext2_node *victim, const struct ext2_scan *nsc, uint32_t nat)
{
    struct ext2_mount *em = r->em;
    struct ext2_node *od = r->od;
    struct ext2_node *nd = r->nd;
    struct ext2_scan osc;
    uint32_t oat, oprev;
    int rc;

    ext2_node_nlink_set(n, n->nlink + 1);
    ext2_stamp(n, T_CTIME);
    rc = ext2_inode_store(em, n);
    if (rc != 0) {
        ext2_node_nlink_set(n, n->nlink - 1);
        return rc;
    }
    if (victim) {
        uint64_t off;
        uint8_t raw[8];

        rc = dir_block_at(em, nd, nat, &off);
        if (rc == 0) {
            rc = dir_settle(em, nd);
        }
        if (rc == 0) {
            dir_changed(nd);
            LE32_PUT(raw, n->ino);
            LE16_PUT(raw + 4, nsc->rec_len);
            raw[6] = (uint8_t)nsc->name_len;
            raw[7] = (em->incompat & EXT2_INCOMPAT_FILETYPE) ? (uint8_t)ext2_ftype_of(n->mode) : 0;
            rc = ext2_write_bytes(em, off, raw, 8, WRITE_THROUGH | WRITE_KIND(READ_DATA));
        }
        if (rc == 0) {
            rc = dir_commit(em, off, 8);
        }
        if (rc != 0) {
            return rc;
        }
        child_drop_link(em, victim);
    } else {
        rc = dir_add(em, nd, r->newname, n->ino, ext2_ftype_of(n->mode));
        if (rc != 0) {
            ext2_node_nlink_set(n, n->nlink - 1);
            return rc;
        }
    }
    rc = dir_find(em, od, r->oldname, &oat, &oprev, &osc);
    if (rc <= 0) {
        return rc < 0 ? rc : -ENOENT;
    }
    rc = dir_remove_at(em, od, oat, oprev, &osc);
    if (rc != 0) {
        return rc;
    }
    ext2_node_nlink_set(n, n->nlink - 1);
    (void)ext2_inode_store(em, n);
    ext2_stamp(od, T_MTIME | T_CTIME);
    (void)ext2_inode_store(em, od);
    if (od != nd) {
        EXT2_NODE_SET(nd, mtime,
                      ext2_time_fit(nd, EXT2_MTIME_EXTRA + 4, od->mtime.sec, od->mtime.nsec));
        EXT2_NODE_SET(nd, ctime,
                      ext2_time_fit(nd, EXT2_CTIME_EXTRA + 4, od->mtime.sec, od->mtime.nsec));
        (void)ext2_inode_store(em, nd);
    }
    return 0;
}

/**
 * The `rename` of `vfs_ops`: `oldname` in `olddir` to `newname` in `newdir`,
 * either directory NULL for the root. The batch is drained first. The
 * caller holds both directories' locks, and for a move between two of them
 * the mount's rename lock, under which no directory changes its parent.
 *
 * The source's and the victim's nodes are those every holder shares, had
 * with the directories' locks held, so that each is the inode its entry
 * names. The cases the VFS leaves unlocked are refused first
 * (`rename_check`); then both are locked, the lower inode number first,
 * which takes again the locks the VFS took, and a victim directory is
 * checked empty under its own lock.
 *
 * A file: the count raised, the new entry written, the old entry removed,
 * the count lowered, so that the file has a name at every stop. A
 * destination file is replaced by rewriting its entry to the source's
 * inode, after which the victim loses a count and goes as an unlink's does.
 * A directory moves by a new entry, its `..` rewritten, the new parent's
 * count raised, the old entry removed and the old parent's count lowered; a
 * destination directory must be empty and goes first, as rmdir's does. The
 * old entry is found again before it is removed, since a removal in the
 * same directory may have moved it. Two names of one file change nothing.
 *
 * Returns 0; -ENOENT for a missing source or a removed destination
 * directory, which takes no entry; -EISDIR, -ENOTDIR, -ENOTEMPTY; -EINVAL
 * for a directory moved below itself, found by a walk up `..` before any
 * write; -EMLINK; -ENAMETOOLONG; -EROFS; -ENOSPC; -EIO, with nothing
 * written, when either directory, the inode moved or the one it replaces is
 * fenced (`n->fenced`); or -errno.
 */
int ext2_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                const char *newname)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *od = olddir ? olddir : mnt->root;
    struct ext2_node *nd = newdir ? newdir : mnt->root;
    struct ext2_node *n, *victim = 0, *first, *second;
    struct ext2_rename_names names;
    struct ext2_scan osc, nsc;
    uint32_t oat, oprev, nat = 0, nprev = 0;
    int rc, have_new;

    names.em = em;
    names.od = od;
    names.oldname = oldname;
    names.nd = nd;
    names.newname = newname;
    rc = ext2_assert_writable(mnt, em);
    if (rc != 0) {
        return rc;
    }
    ext2_assert_node_locked(od);
    ext2_assert_node_locked(nd);
    if (od != nd) {
        sleep_lock_assert(&mnt->rename, "ext2: a move between directories outside the rename lock");
    }
    if (k_strlen(newname) > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    if (nd->unlinked) {
        return -ENOENT;
    }
    if (od->fenced || nd->fenced) {
        return -EIO;
    }
    rc = dir_find(em, od, oldname, &oat, &oprev, &osc);
    if (rc <= 0) {
        return rc < 0 ? rc : -ENOENT;
    }
    have_new = dir_find(em, nd, newname, &nat, &nprev, &nsc);
    if (have_new < 0) {
        return have_new;
    }
    if (have_new && nsc.ino == osc.ino) {
        return 0;
    }
    n = ext2_node_for(em, osc.ino, &rc);
    if (!n) {
        return rc == -ESTALE ? ext2_stale(em, osc.ino) : rc;
    }
    if (have_new) {
        victim = ext2_node_for(em, nsc.ino, &rc);
        if (!victim) {
            ext2_put(mnt, n);
            return rc == -ESTALE ? ext2_stale(em, nsc.ino) : rc;
        }
    }
    rc = rename_check(em, od, nd, n, victim);
    if (rc == 0) {
        first = victim && victim->ino < n->ino ? victim : n;
        second = first == n ? victim : n;
        ext2_node_lock(first);
        if (second) {
            ext2_node_lock(second);
        }
        if (n->fenced || (victim && victim->fenced)) {
            rc = -EIO;
        } else if (victim && (victim->mode & S_IFMT_) == S_IFDIR_) {
            int empty;

            rc = dir_is_empty(em, victim, &empty);
            if (rc == 0 && !empty) {
                rc = -ENOTEMPTY;
            }
        }
        if (rc == 0 && (n->mode & S_IFMT_) == S_IFDIR_) {
            rc = rename_dir(&names, n, victim, &nsc, nat, nprev);
        } else if (rc == 0) {
            rc = rename_file(&names, n, victim, &nsc, nat);
        }
        if (second) {
            ext2_node_unlock(second);
        }
        ext2_node_unlock(first);
    }
    if (victim) {
        ext2_put(mnt, victim);
    }
    ext2_put(mnt, n);
    return rc;
}
