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
};

/* A copy of one block of a directory, for a scan that reads all of a block's
 * entries: the block is read once and not twice for each entry. `data` is
 * NULL for a scan without one, and `index` is the copy's block of the
 * directory, or -1 before the first read. */
struct ext2_window {
    uint8_t *data;
    int64_t index;
};

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
 * corrupt record whose header runs past its block. 0, or -EIO.
 */
static int dir_bytes(struct ext2_mount *em, const struct ext2_node *d, struct ext2_window *w,
                     uint32_t at, void *dst, uint32_t len)
{
    int64_t index = (int64_t)(at >> em->block_shift);
    int rc;

    if (!w || !w->data || (at & (em->block_size - 1)) + len > em->block_size) {
        return ext2_read_data(em, d, at, dst, len);
    }
    if (w->index != index) {
        w->index = -1;
        rc = ext2_read_data(em, d, (uint64_t)index << em->block_shift, w->data, em->block_size);
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
 * directory's size, a name past its record or an inode past the count, is
 * -EIO with the error bit set and a record. An entry with inode 0 is
 * returned with that inode and no name, and the caller skips it. With window
 * `w` the entry is read from a copy of its block, which the caller must not
 * write while it scans.
 */
static int dir_scan_in(struct ext2_mount *em, const struct ext2_node *d, struct ext2_window *w,
                       uint32_t *cursor, struct ext2_scan *sc)
{
    uint8_t head[8];
    uint32_t at = *cursor;
    uint32_t in;
    int rc;

    if (at >= d->size) {
        return 0;
    }
    in = at % em->block_size;
    rc = dir_bytes(em, d, w, at, head, 8);
    if (rc != 0) {
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

/** Whether the strings `a` and `b` are equal. */
static int names_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The lookup-hint slot of directory inode `dir`, or -1 when it has none. */
static int lookup_slot(const struct ext2_mount *em, uint32_t dir)
{
    int i;

    for (i = 0; i < EXT2_LOOKUP_HINTS; i++) {
        if (em->lookup_hint[i].dir == dir) {
            return i;
        }
    }
    return -1;
}

/**
 * Find `name` in directory `d`, whose lock the caller holds, for a lookup:
 * 1 with the inode it names in `*ino` and the entry's byte in `*at`, 0 when
 * it is absent, or -EIO.
 *
 * A directory of this format has no index, so the lookup reads entries
 * until it finds the name. It starts at the block where the last lookup in
 * the same directory found its name and wraps around to it, as Linux's
 * ext2 does: `ls` and `find` look up the names of a directory in the order
 * it lists them, so each is found near the one before, and a directory of
 * n names is listed in time proportional to n and not to n squared. A block
 * starts an entry whatever was written since, since no entry crosses one.
 */
static int dir_lookup(struct ext2_mount *em, const struct ext2_node *d, const char *name,
                      uint32_t *ino, uint32_t *at)
{
    uint32_t start = 0, cursor, limit;
    struct ext2_window w;
    int pass, slot;

    slot = lookup_slot(em, d->ino);
    if (slot >= 0 && ((uint64_t)em->lookup_hint[slot].block << em->block_shift) < d->size) {
        start = em->lookup_hint[slot].block << em->block_shift;
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
                if (slot < 0) {
                    slot = em->lookup_next;
                    em->lookup_next = (em->lookup_next + 1) % EXT2_LOOKUP_HINTS;
                    em->lookup_hint[slot].dir = d->ino;
                }
                em->lookup_hint[slot].block = *at >> em->block_shift;
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
 * as `name`, read under `d`'s lock: 1, 0, or -EIO. */
static int dir_names_at(struct ext2_mount *em, struct ext2_node *d, uint32_t at, const char *name,
                        uint32_t ino)
{
    struct ext2_scan sc;
    uint32_t cursor = at;
    int got;

    ext2_node_lock(d);
    got = dir_scan(em, d, &cursor, &sc);
    ext2_node_unlock(d);
    return got <= 0 ? got : sc.ino == ino && names_equal(sc.name, name);
}

/**
 * The `lookup` of `vfs_ops`: `name` in directory `dir`, the mount's root when
 * it is NULL. 0 with the node held, not locked; -ENOTDIR, -ENOENT, -ENOMEM,
 * or -EIO.
 *
 * The entry is found under the directory's lock, which the caller may hold
 * already, and the node is had with the lock let go, so that the read of a
 * cold inode holds up no other lookup in the directory and a second lookup
 * of the name waits on the load itself (`ext2_node_for`). When the lock did go,
 * the entry is read again under it: a name removed or moved meanwhile, whose
 * inode may have been freed or given to another file, is looked up anew.
 */
int ext2_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *d = dir ? dir : mnt->root;
    struct ext2_node *n;
    uint32_t ino = 0, at = 0;
    int rc, kept;

    if ((d->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    for (;;) {
        if (name[0] == '.' && name[1] == '\0') {
            rc = 1;
            ino = d->ino;
        } else {
            ext2_node_lock(d);
            rc = dir_lookup(em, d, name, &ino, &at);
            ext2_node_unlock(d);
        }
        if (rc <= 0) {
            return rc < 0 ? rc : -ENOENT;
        }
        if (ino == d->ino) {
            ext2_node_ref(d, 1);
            *node = d;
            return 0;
        }
        kept = sleep_lock_held(&d->lock);
        n = ext2_node_for(em, ino, &rc);
        if (!n) {
            return rc;
        }
        rc = kept ? 1 : dir_names_at(em, d, at, name, ino);
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
 * cursor past it, 0 at the end, -ENOENT for a removed directory, or -EIO.
 */
static int dir_next(struct ext2_mount *em, struct ext2_node *d, uint32_t *cursor,
                    struct vfs_dirent *ent)
{
    if (d->unlinked) {
        return -ENOENT;
    }
    for (;;) {
        struct ext2_scan sc;
        int got = dir_scan(em, d, cursor, &sc);

        if (got <= 0) {
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
                return rc;
            }
            ent->is_dir = (probe.mode & S_IFMT_) == S_IFDIR_;
            ent->type = (int)VFS_DT_OF(probe.mode);
        }
        return 1;
    }
}

/**
 * The `readdir` of `vfs_ops`: the next entry with an inode of directory
 * `handle` at `*cursor`, a byte offset, `.` and `..` included. Its type comes
 * from the entry's file-type byte when the volume has the filetype feature,
 * and from the inode's mode otherwise, which costs a read of the inode. 1
 * with `ent` filled, 0 at the end, -ENOTDIR, -ENOENT for a directory removed
 * while held, or -EIO. Takes the directory's lock (`dir_next`), and the big
 * lock while the batch is not empty (`ext2_batch_guard`).
 */
int ext2_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent)
{
    struct ext2_node *d = handle;
    int rc, big;

    if ((d->mode & S_IFMT_) != S_IFDIR_) {
        return -ENOTDIR;
    }
    ext2_node_lock(d);
    big = ext2_batch_guard(ext2_em_of(mnt));
    rc = dir_next(ext2_em_of(mnt), d, cursor, ent);
    ext2_node_unlock(d);
    if (big) {
        bkl_exit();
    }
    return rc;
}

/**
 * The target of symbolic link `n` into `buf`, as `ext2_readlink` gives it,
 * for a caller that holds `n`'s lock, or a hold alone on a fast link
 * (`fast_target`), whose words do not change.
 */
static int link_target(struct ext2_mount *em, const struct ext2_node *n, char *buf, uint32_t cap)
{
    uint32_t len;

    if (n->size >= em->block_size) {
        return -EIO;
    }
    len = (uint32_t)n->size;
    if (len + 1 > cap) {
        return -ENAMETOOLONG;
    }
    if (n->fast_target) {
        uint32_t i;

        if (len >= EXT2_FAST_LINK) {
            return -EIO; /* the pointer words hold at most 59 bytes */
        }
        for (i = 0; i < len; i++) {
            buf[i] = (char)((n->block[i / 4] >> ((i % 4) * 8)) & 0xFFu);
        }
    } else {
        int rc;

        if (n->block[0] == 0) {
            return -EIO;
        }
        rc = ext2_read_data(em, n, 0, buf, len);
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
 * read. Takes the link's lock (`link_target`), and the big lock while the
 * batch is not empty (`ext2_batch_guard`), as a read does.
 */
int ext2_readlink(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct ext2_node *n = node;
    int rc, big;

    if ((n->mode & S_IFMT_) != S_IFLNK_) {
        return -EINVAL;
    }
    ext2_node_lock(n);
    big = ext2_batch_guard(ext2_em_of(mnt));
    rc = link_target(ext2_em_of(mnt), n, buf, cap);
    ext2_node_unlock(n);
    if (big) {
        bkl_exit();
    }
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
 * lock.
 */
int ext2_readlink_cached(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap)
{
    struct ext2_mount *em = ext2_em_of(mnt);
    struct ext2_node *n = node;

    if (!n->fast_target || n->size >= EXT2_FAST_LINK || n->size + 1 > cap) {
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
 * go before the next step, so the walk holds one directory at a time.
 */
static int path_step(struct ext2_mount *em, uint32_t ino, uint32_t *cursor, struct ext2_scan *sc,
                     int *is_dir)
{
    int got;
    struct ext2_node *d;

    *is_dir = 0;
    d = ext2_node_for(em, ino, &got);
    if (!d) {
        return got;
    }
    ext2_node_lock(d);
    got = dir_live(d) ? dir_scan(em, d, cursor, sc) : 0;
    if (got > 0 && sc->ino != 0) {
        if (em->incompat & EXT2_INCOMPAT_FILETYPE) {
            *is_dir = sc->type == EXT2_FT_DIR;
        } else {
            struct ext2_node child;

            got = ext2_inode_load(em, sc->ino, &child);
            got = got != 0 ? got : 1;
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
 * is a directory still. 0, -ENOENT for a removed directory or one without
 * `..`, or -errno.
 */
static int dir_parent(struct ext2_mount *em, struct ext2_node *cur, struct ext2_node **parent)
{
    struct ext2_scan sc;
    uint32_t cursor = 0;
    int rc = 0;

    ext2_node_lock(cur);
    while (!cur->unlinked) {
        rc = dir_scan(em, cur, &cursor, &sc);
        if (rc <= 0 || (sc.ino != 0 && names_equal(sc.name, ".."))) {
            break;
        }
    }
    ext2_node_unlock(cur);
    if (rc == 1) {
        *parent = ext2_node_for(em, sc.ino, &rc);
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
 * map, into `*off`; 0, -EIO for a hole, or the errors of `ext2_block_at`. */
static int dir_block_at(struct ext2_mount *em, const struct ext2_node *d, uint32_t at,
                        uint64_t *off)
{
    uint32_t b;
    int rc = ext2_block_at(em, d, at / em->block_size, &b);

    if (rc != 0) {
        return rc;
    }
    if (b == 0) {
        return -EIO;
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

    return ext2_write_bytes(em, off, raw, len, WRITE_THROUGH);
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
        return ext2_write_bytes(em, off, pair, used + len, WRITE_THROUGH);
    }
    rc = ext2_write_bytes(em, off + used, entry, len, WRITE_THROUGH);
    if (rc != 0) {
        return rc;
    }
    used = dirent_encode(pair, old->ino, used, old->name, old->name_len, old->type);
    return ext2_write_bytes(em, off, pair, used, WRITE_THROUGH);
}

/**
 * Clear the hashed-index flag of directory `d`, whose entries are about to
 * change, and store its inode with WRITE_THROUGH before they do. 0, or
 * -errno.
 *
 * This driver keeps the linear entries only, which an index leaves readable
 * as they are: the index root lies in the slack of `..`, and each index
 * block is one empty entry spanning it. An index left set over changed
 * entries would be stale, so the flag goes, as Linux's ext2 drops it on
 * every change (fs/ext2/dir.c:468, 555 and 609 in Linux 6.12.115), and
 * e2fsck -D builds the index again. Storing the inode first means that no
 * stop leaves a changed directory marked indexed.
 *
 * A failed store, such as a failed read of the inode table before any
 * write, puts the flag back in memory, so that the next change stores the
 * cleared flag again before it touches an entry; a flag left cleared in
 * memory would let that change skip the store and write over the index
 * root in the slack of `..` while the disk still marks it indexed. Linux's
 * ext2 has no such store to fail: it clears i_flags in memory as it
 * changes the entries, and writeback copies the flags to the disk later
 * (fs/ext2/dir.c:468 and fs/ext2/inode.c:1567 in Linux 6.12.115). Should
 * the failed store have reached the disk, the entries are unchanged, and
 * the next store of the inode marks it indexed again, which is still true.
 */
static int dir_unindex(struct ext2_mount *em, struct ext2_node *d)
{
    int rc;

    if (!(d->flags & EXT2_INDEX_FL)) {
        return 0;
    }
    d->flags &= ~EXT2_INDEX_FL;
    rc = ext2_inode_store(em, d);
    if (rc != 0) {
        d->flags |= EXT2_INDEX_FL;
    }
    return rc;
}

/**
 * Add `name` naming `ino`, of file type `ftype`, to directory `d`: in the
 * first empty record with room, whole, or the first entry with room behind
 * its name, split; else in a new block, allocated, written zeroed with one
 * empty entry spanning it, and chained with `i_blocks` and covered by the
 * size in one write of the inode. The entry itself is the last write. 0,
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
    int rc, fresh;

    if (d->unlinked) {
        return -ENOENT;
    }
    if (name_len == 0 || name_len > EXT2_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    rc = dir_unindex(em, d);
    if (rc != 0) {
        return rc;
    }
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
            return dirent_write(em, off, ino, sc.rec_len, name, name_len, ftype);
        }
        used = dirent_encode(entry, ino, room, name, name_len,
                             (em->incompat & EXT2_INCOMPAT_FILETYPE) ? ftype : 0);
        return dirent_split(em, off, &sc, entry, used);
    }
    window_close(&w);
    {
        uint32_t index = (uint32_t)(d->size >> em->block_shift);
        uint32_t b;
        uint64_t off, free;
        uint8_t head[8];

        if (d->size + em->block_size > 0xFFFFFFFFu) {
            return -EFBIG;
        }
        free = ext2_free_blocks(em);
        if (1 + ext2_pointer_levels(em, index) > free ||
            (!ext2_may_use_reserved() &&
             free - (1 + ext2_pointer_levels(em, index)) < em->r_blocks)) {
            return -ENOSPC;
        }
        rc = ext2_map_alloc(em, d, index, &b, &fresh);
        if (rc != 0) {
            return rc;
        }
        off = (uint64_t)b * em->block_size;
        rc = ext2_write_bytes(em, off, 0, em->block_size, 0);
        if (rc != 0) {
            return rc;
        }
        LE32_PUT(head, 0);
        LE16_PUT(head + 4, em->block_size);
        head[6] = 0;
        head[7] = 0;
        rc = ext2_write_bytes(em, off, head, 8, 1);
        if (rc != 0) {
            return rc;
        }
        ext2_node_size_set(d, d->size + em->block_size);
        ext2_stamp(d, T_MTIME | T_CTIME);
        rc = ext2_inode_store(em, d);
        if (rc != 0) {
            return rc;
        }
        d->add_from = index << em->block_shift;
        return dirent_write(em, off, ino, em->block_size, name, name_len, ftype);
    }
}

/** Remove the entry `sc` at byte `at` of `d`, whose predecessor in its block
 * is at `prev_at`: the first in its block has its inode cleared, and any
 * other is merged into its predecessor's record, with WRITE_THROUGH. 0, or
 * -errno. */
static int dir_remove_at(struct ext2_mount *em, struct ext2_node *d, uint32_t at, uint32_t prev_at,
                         const struct ext2_scan *sc)
{
    uint64_t off;
    int rc = dir_unindex(em, d);

    if (rc != 0) {
        return rc;
    }
    if (at % em->block_size == 0) {
        uint8_t zero[4] = {0, 0, 0, 0};

        rc = dir_block_at(em, d, at, &off);
        if (rc != 0) {
            return rc;
        }
        return ext2_write_bytes(em, off, zero, 4, 1);
    } else {
        struct ext2_scan ps;
        uint32_t cursor = prev_at;
        uint8_t raw[2];
        int got = dir_scan(em, d, &cursor, &ps);

        if (got <= 0) {
            return got < 0 ? got : -EIO;
        }
        rc = dir_block_at(em, d, prev_at, &off);
        if (rc != 0) {
            return rc;
        }
        LE16_PUT(raw, ps.rec_len + sc->rec_len);
        return ext2_write_bytes(em, off + 4, raw, 2, 1);
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
 * -EIO for a directory without one, or -errno. */
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
            return got < 0 ? got : -EIO;
        }
        if (sc.ino != 0 && names_equal(sc.name, "..")) {
            rc = dir_block_at(em, d, here, &off);
            if (rc == 0) {
                rc = dir_unindex(em, d);
            }
            if (rc != 0) {
                return rc;
            }
            LE32_PUT(raw, parent);
            return ext2_write_bytes(em, off, raw, 4, 1);
        }
    }
}

/**
 * Whether directory inode `ino` is `ancestor` or below it, into `*below`,
 * by the `..` entries from `ino` up to the root: 0, -ELOOP past PATH_DEPTH
 * levels, -EIO for a directory without `..`, or -errno.
 *
 * The caller holds the mount's rename lock, under which no directory
 * changes its parent, and may hold the locks of directories on the way;
 * so the walk reads copies of the inodes and takes no lock, since one of
 * an ancestor would come after its descendant's. An entry is changed in
 * one step between sleeps, so a `..` read while another call adds a name
 * beside it is whole.
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
            return rc;
        }
        for (;;) {
            int got = dir_scan(em, &cur, &cursor, &sc);

            if (got < 0) {
                return got;
            }
            if (got == 0) {
                return -EIO;
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
 * written zeroed, for a caller that stores the inode once it has the rest.
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
 * the VFS holds, until it has a directory entry.
 * 0, -ENOSPC, -ENOMEM, or -errno; a failure frees the inode.
 */
static int inode_new(struct ext2_mount *em, const struct ext2_node *dir, uint32_t mode, int store,
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
    n->uid = current_proc ? proc_fsuid(current_proc) : 0;
    n->gid = current_proc ? proc_fsgid(current_proc) : 0;
    if (dir->mode & S_ISGID_) {
        n->gid = dir->gid;
        if ((mode & S_IFMT_) == S_IFDIR_) {
            mode |= S_ISGID_;
        }
    }
    n->mode = mode;
    n->nlink = (mode & S_IFMT_) == S_IFDIR_ ? 2 : 1;
    n->extra_isize = em->new_extra;
    ext2_stamp(n, T_ATIME | T_MTIME | T_CTIME);
    if (ext2_extra_has(n, EXT2_CRTIME + 4)) {
        n->crtime = ext2_time_fit(n, EXT2_CRTIME_EXTRA + 4, n->ctime.sec, n->ctime.nsec);
    }
    n->generation = ext2_generation_next(em);
    rc = store ? ext2_inode_write(em, n, 1, WRITE_THROUGH)
               : ext2_write_bytes(em, ext2_inode_offset(em, n->ino), 0, em->inode_size,
                                  WRITE_THROUGH);
    if (rc != 0) {
        ext2_inode_bit_free(em, n->ino, is_dir);
        ext2_node_settle(em, n, rc);
        return rc;
    }
    ext2_node_settle(em, n, 0);
    *out = n;
    return 0;
}

/**
 * The node of inode `ino`, which an entry of a directory whose lock the
 * caller holds names, held and locked, into `*out`: the one node every
 * holder of the inode shares (`ext2_node_for`), never a copy, so that a change
 * made here is the one an open file sees. The VFS locks the node of the
 * name in unlink, rmdir and rename before it calls the type, and this takes
 * that lock again by depth. 0, or the errors of `ext2_node_for`.
 */
static int child_take(struct ext2_mount *em, uint32_t ino, struct ext2_node **out)
{
    int rc;
    struct ext2_node *c = ext2_node_for(em, ino, &rc);

    if (!c) {
        return rc;
    }
    ext2_node_lock(c);
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
 * Mark node `c`, whose lock the caller holds and whose link count is 0,
 * as having no name, with its change time set: freed at once when the
 * caller's is its only hold, since nothing else can reach it, and
 * otherwise stored with its count of 0, for e2fsck to read it as deleted
 * after a crash, and freed at its last put. 0, or -errno.
 */
static int child_unlinked(struct ext2_mount *em, struct ext2_node *c)
{
    ext2_stamp(c, T_CTIME);
    c->unlinked = 1;
    if (c->refs == 1) {
        return ext2_release_inode(em, c);
    }
    return ext2_inode_store(em, c);
}

/**
 * The `create` of `vfs_ops`: a new regular file `name` in `dir`, the root for
 * NULL, with the permission bits of `mode`: the inode allocated and written
 * whole, then the entry that names it, then the directory's times into the
 * cache. A failure to add the entry frees the inode; a failure to store the
 * times sets the error bit and is not the call's, since the entry is there.
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
        n->nlink = 0;
        ext2_release_inode(em, n);
        ext2_node_unlock(n);
        ext2_node_unhold(em, n);
        return rc;
    }
    ext2_stamp(d, T_MTIME | T_CTIME);
    rc = ext2_inode_times_later(em, d);
    if (rc != 0) {
        ext2_mark_error(em);
    }
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
            n->block[0] = (major << 8) | minor;
        } else {
            n->block[1] = (minor & 0xFFu) | (major << 8) | ((minor & 0xFFF00u) << 12);
        }
    }
    rc = ext2_inode_store(em, n);
    if (rc == 0) {
        rc = dir_add(em, d, name, n->ino, ext2_ftype_of(n->mode));
    }
    if (rc != 0) {
        n->nlink = 0;
        ext2_release_inode(em, n);
        ext2_node_unlock(n);
        ext2_node_unhold(em, n);
        return rc;
    }
    ext2_stamp(d, T_MTIME | T_CTIME);
    rc = ext2_inode_times_later(em, d);
    if (rc != 0) {
        ext2_mark_error(em);
    }
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
 * it as deleted (`child_unlinked`). The batch is drained first. The caller
 * holds the directory's lock. 0, -ENOENT, -EISDIR, -EROFS, or -errno.
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
        rc = ext2_inode_times_later(em, d);
    }
    if (rc == 0) {
        if (n->nlink > 0) {
            n->nlink--;
        }
        if (n->nlink == 0) {
            rc = child_unlinked(em, n);
        } else {
            ext2_stamp(n, T_CTIME);
            rc = ext2_inode_store(em, n);
        }
    }
    child_give(em, n);
    return rc;
}

/**
 * The `mkdir` of `vfs_ops`: a new directory `name` in `dir`, with the
 * permission bits of `mode`. Its block is written with `.` and `..`, then
 * its inode with a count of two, then the parent's raised count, and the
 * parent's entry last; a stop before the inode leaves a set bit with
 * nothing behind it. A failure frees the new inode, and lowers the parent's
 * count again when the entry was not written. The batch is drained first.
 * The caller holds the parent's lock, and the new directory's is held
 * until it is named. 0, -ENOENT for a removed parent, -EEXIST, -EMLINK at
 * 65000 links, -ENAMETOOLONG, -EROFS, -ENOSPC, -ENOMEM, or -errno.
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
        rc = ext2_write_bytes(em, (uint64_t)b * em->block_size, 0, em->block_size, 0);
    }
    if (rc == 0) {
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
        rc = ext2_write_bytes(em, (uint64_t)b * em->block_size, head, 24, 1);
    }
    if (rc == 0) {
        ext2_node_size_set(n, em->block_size);
        rc = ext2_inode_store(em, n);
    }
    if (rc == 0) {
        d->nlink++;
        ext2_stamp(d, T_MTIME | T_CTIME);
        rc = ext2_inode_store(em, d);
    }
    if (rc == 0) {
        rc = dir_add(em, d, name, n->ino, 2);
        if (rc != 0) {
            d->nlink--;
            (void)ext2_inode_store(em, d);
        }
    }
    if (rc != 0) {
        n->nlink = 0;
        ext2_release_inode(em, n);
        ext2_node_unlock(n);
        ext2_node_unhold(em, n);
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
 * Linux. The batch is drained first. The caller holds the parent's lock.
 * 0, -ENOENT, -ENOTDIR, -ENOTEMPTY, -EROFS, or -errno.
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
        c->nlink = 0;
        c->unlinked = 1;
        if (d->nlink > 2) {
            d->nlink--;
        }
        ext2_stamp(d, T_MTIME | T_CTIME);
        rc = ext2_inode_store(em, d);
    }
    if (rc == 0) {
        rc = child_unlinked(em, c);
    }
    child_give(em, c);
    return rc;
}

/**
 * The name `name` in directory `d` for the file `n`, for `ext2_link`, whose
 * caller holds both locks: the count raised and stored, then the name
 * written, then the directory's times. 0, -ENOENT for a file with no name
 * left, -EMLINK, -EEXIST, or -errno.
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
    n->nlink++;
    ext2_stamp(n, T_CTIME);
    rc = ext2_inode_store(em, n);
    if (rc != 0) {
        n->nlink--;
        return rc;
    }
    rc = dir_add(em, d, name, n->ino, ext2_ftype_of(n->mode));
    if (rc != 0) {
        n->nlink--;
        ext2_inode_store(em, n);
        return rc;
    }
    ext2_stamp(d, T_MTIME | T_CTIME);
    return ext2_inode_times_later(em, d);
}

/**
 * The `link` of `vfs_ops`: a new name `name` in `dir` for the file `node`:
 * the count raised first, then the name written, so that a stop between
 * leaves a count one too high, which is a count repair; then the
 * directory's times. A failure to write the name lowers the count again.
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
 * shorter than 60 bytes is kept in the inode's pointer words, and a longer
 * one in a block of its own, written first; then the inode, then the entry,
 * then the directory's times. A failure frees the inode. The batch is
 * drained first. The caller holds the directory's lock, and the link's is
 * held until it is named. 0, -ENOENT for a removed directory,
 * -ENAMETOOLONG for a name past 255 bytes or for a target that is empty or
 * of a block or more, -EEXIST, -EROFS, -ENOSPC, -ENOMEM, or -errno.
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
            n->block[i / 4] |= (uint32_t)(uint8_t)target[i] << ((i % 4) * 8);
        }
        n->fast_target = 1;
    } else {
        uint32_t b;
        int fresh;

        rc = ext2_map_alloc(em, n, 0, &b, &fresh);
        if (rc == 0) {
            rc = ext2_write_bytes(em, (uint64_t)b * em->block_size, 0, em->block_size, 0);
        }
        if (rc == 0) {
            rc = ext2_write_bytes(em, (uint64_t)b * em->block_size, target, tlen, 1);
        }
    }
    if (rc == 0) {
        ext2_node_size_set(n, tlen);
        rc = ext2_inode_store(em, n);
    }
    if (rc == 0) {
        rc = dir_add(em, d, name, n->ino, 7);
    }
    if (rc == 0) {
        ext2_stamp(d, T_MTIME | T_CTIME);
        rc = ext2_inode_times_later(em, d);
    }
    if (rc != 0) {
        n->nlink = 0;
        ext2_release_inode(em, n);
        ext2_node_unlock(n);
        ext2_node_unhold(em, n);
        return rc;
    }
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
 * victim removed as rmdir removes it, then the new entry, then `..` rewritten
 * and the new parent's count raised when the parents differ, then the old
 * entry removed and the old parent's count lowered. 0, -ENOENT, -EMLINK, or
 * -errno.
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

    if (od != nd && nd->nlink >= 65000) {
        return -EMLINK;
    }
    if (victim) {
        rc = dir_remove_at(em, nd, nat, nprev, nsc);
        if (rc != 0) {
            return rc;
        }
        victim->nlink = 0;
        victim->unlinked = 1;
        if (nd->nlink > 2) {
            nd->nlink--;
        }
        rc = ext2_inode_store(em, nd);
        if (rc == 0) {
            rc = child_unlinked(em, victim);
        }
        if (rc != 0) {
            return rc;
        }
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
        nd->nlink++;
        ext2_stamp(nd, T_MTIME | T_CTIME);
        rc = ext2_inode_store(em, nd);
        if (rc != 0) {
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
    if (od != nd && od->nlink > 2) {
        od->nlink--;
    }
    ext2_stamp(od, T_MTIME | T_CTIME);
    return ext2_inode_store(em, od);
}

/**
 * The rename of file `n` from `r->oldname` in `r->od` to `r->newname` in
 * `r->nd`, over the file `victim` at `nat` when it is not NULL, for
 * `ext2_rename`, whose caller holds every lock: the count raised, the new
 * entry written or the victim's entry pointed at `n`, the victim's count
 * lowered as unlink lowers it, the old entry removed and the count lowered
 * again. 0, -ENOENT, or -errno.
 */
static int rename_file(const struct ext2_rename_names *r, struct ext2_node *n,
                       struct ext2_node *victim, uint32_t nat)
{
    struct ext2_mount *em = r->em;
    struct ext2_node *od = r->od;
    struct ext2_node *nd = r->nd;
    struct ext2_scan osc;
    uint32_t oat, oprev;
    int rc;

    n->nlink++;
    ext2_stamp(n, T_CTIME);
    rc = ext2_inode_store(em, n);
    if (rc != 0) {
        n->nlink--;
        return rc;
    }
    if (victim) {
        uint64_t off;
        uint8_t raw[4];

        rc = dir_block_at(em, nd, nat, &off);
        if (rc == 0) {
            rc = dir_unindex(em, nd);
        }
        if (rc == 0) {
            LE32_PUT(raw, n->ino);
            rc = ext2_write_bytes(em, off, raw, 4, 1);
        }
        if (rc == 0 && (em->incompat & EXT2_INCOMPAT_FILETYPE)) {
            uint8_t ft = (uint8_t)ext2_ftype_of(n->mode);

            rc = ext2_write_bytes(em, off + 7, &ft, 1, 1);
        }
        if (rc != 0) {
            return rc;
        }
        if (victim->nlink > 0) {
            victim->nlink--;
        }
        if (victim->nlink == 0) {
            rc = child_unlinked(em, victim);
        } else {
            ext2_stamp(victim, T_CTIME);
            rc = ext2_inode_store(em, victim);
        }
        if (rc != 0) {
            return rc;
        }
    } else {
        rc = dir_add(em, nd, r->newname, n->ino, ext2_ftype_of(n->mode));
        if (rc != 0) {
            n->nlink--;
            (void)ext2_inode_store(em, n);
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
    n->nlink--;
    rc = ext2_inode_store(em, n);
    if (rc != 0) {
        return rc;
    }
    ext2_stamp(od, T_MTIME | T_CTIME);
    rc = ext2_inode_store(em, od);
    if (rc == 0 && od != nd) {
        nd->mtime = ext2_time_fit(nd, EXT2_MTIME_EXTRA + 4, od->mtime.sec, od->mtime.nsec);
        nd->ctime = ext2_time_fit(nd, EXT2_CTIME_EXTRA + 4, od->mtime.sec, od->mtime.nsec);
        rc = ext2_inode_store(em, nd);
    }
    return rc;
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
 * write; -EMLINK; -ENAMETOOLONG; -EROFS; -ENOSPC; or -errno.
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
        return rc;
    }
    if (have_new) {
        victim = ext2_node_for(em, nsc.ino, &rc);
        if (!victim) {
            ext2_put(mnt, n);
            return rc;
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
        if (victim && (victim->mode & S_IFMT_) == S_IFDIR_) {
            int empty;

            rc = dir_is_empty(em, victim, &empty);
            if (rc == 0 && !empty) {
                rc = -ENOTEMPTY;
            }
        }
        if (rc == 0 && (n->mode & S_IFMT_) == S_IFDIR_) {
            rc = rename_dir(&names, n, victim, &nsc, nat, nprev);
        } else if (rc == 0) {
            rc = rename_file(&names, n, victim, nat);
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
