/*
 * ext2_check.c: the check of an ext2 volume that was not unmounted cleanly, at
 * its writable mount, which repairs nothing but free counts.
 */

#include "ext2_internal.h"

/* ---- the mount-time check ---- */

/*
 * A volume whose valid bit is clear was not unmounted, and one whose error
 * bit is set was found faulty by an earlier mount. Either is checked
 * before it is written, under the walk lock and the shared bitmap the FAT
 * drivers use: the structural blocks are seeded into the bitmap, the tree
 * is walked from the root and every block every reached inode names is
 * marked, its extended-attribute block among them, the inodes the inode
 * bitmap calls used but the walk did not reach are scanned as orphans, and
 * the bitmaps are compared with the marks. Nothing is repaired. Lost
 * blocks, orphans and shared attribute blocks whose reference count is
 * above the inodes that name them are counted and kept, held out of the
 * allocator by the bits and counts they still have, and the error bit is
 * set so that the volume is not marked clean again before `e2fsck` has
 * run. A fault names the first one met and sets the error bit: a block
 * claimed twice or out of range, an inode whose pointers do not match its
 * size or its `i_blocks`, an attribute block of the wrong form or named by
 * more inodes than its count, a directory with two parents or without its
 * parent, an entry of the wrong form, a link count that does not match the
 * entries, a used block or a reached inode the bitmap calls free. The
 * volume mounts writable all the same, as Linux's ext2 mounts a volume
 * whose error bit is set with a warning (`ext2_setup_super`). The group
 * descriptors' free counts are recomputed from the bitmaps rather than
 * taken from the disk, since a crash leaves them stale.
 *
 * The walk keeps its state on the heap rather than on the stack: the node
 * of each directory level, the hard links and the shared attribute blocks
 * met with their expected and found counts, the reached-inode set, one bit
 * per inode of the volume, and one bitmap block; the block bitmap is the
 * one the FAT walks share. A dirty volume whose set or bitmap the free
 * memory cannot hold, a directory deeper than the levels, more shared
 * attribute blocks than its table holds, or hard links the heap cannot
 * record stop the check, which says which: none is a fault of the volume,
 * which mounts writable unchecked, as Linux mounts a volume whose valid
 * bit is clear, and keeps the state that called for the check. Hard-link
 * records are allocated as files are met.
 */
#define EXT2_CHECK_DEPTH        32  /* the directory levels the walk holds */
#define EXT2_CHECK_LINK_BUCKETS 512 /* the index, not a limit on linked files */
#define EXT2_CHECK_ATTRS        512 /* the shared attribute blocks it counts */

/* An extended-attribute block whose header counts more than one reference:
 * the block, that count, and the inodes the check has found naming it. */
struct ext2_attr {
    uint32_t block;
    uint32_t expect, found;
};

/*
 * The check's state: the first fault and its inode; the lost blocks, the
 * orphans, the shared attribute blocks counting more references than the
 * inodes that name them (`stale`), the chained records of `link` and the
 * `attrs` entries of `attr`; the inode of each directory level being
 * walked; the set of reached inodes; and one bitmap block.
 */
struct ext2_check {
    struct ext2_mount *em;
    const char *fault;
    uint32_t fault_ino;
    int limit; /* `fault` is a limit of the check's, not of the volume */
    uint32_t lost, orphans, stale, attrs;
    int link_nomem;
    struct ext2_link *link[EXT2_CHECK_LINK_BUCKETS];
    struct ext2_attr attr[EXT2_CHECK_ATTRS];
    struct ext2_node node[EXT2_CHECK_DEPTH];
    uint8_t *seen; /* `(inodes + 7) / 8` bytes */
    uint8_t bitmap[4096];
};

/**
 * The check's state and both bitmaps, taken together and given back
 * together: the block bitmap through `fat_walk_acquire`, which counts the
 * free frames first, and the state and inode set from the heap. NULL when
 * the heap cannot give any of them, with nothing held.
 */
static struct ext2_check *check_state(struct ext2_mount *em)
{
    struct ext2_check *c;

    if (fat_walk_acquire(em->blocks) != 0) {
        return 0;
    }
    c = kzalloc(sizeof(*c));
    if (c) {
        c->seen = kzalloc((em->inodes + 7) / 8);
    }
    if (!c || !c->seen) {
        fat_walk_release();
        if (c) {
            kfree(c);
        }
        return 0;
    }
    c->em = em;
    return c;
}

/** Free the check's state and inode set; the walk bitmap is given back
 * apart, by `fat_walk_release`. */
static void check_state_free(struct ext2_check *c)
{
    uint32_t i;
    struct ext2_link *link, *next;

    for (i = 0; i < EXT2_CHECK_LINK_BUCKETS; i++) {
        for (link = c->link[i]; link; link = next) {
            next = link->next;
            kfree(link);
        }
    }
    kfree(c->seen);
    kfree(c);
}

/** Record the fault `why` at inode `ino` when it is the first; -EIO, which
 * ends the walk. */
static int check_fault(struct ext2_check *c, uint32_t ino, const char *why)
{
    if (!c->fault) {
        c->fault = why;
        c->fault_ino = ino;
    }
    return -EIO;
}

/** Record that the check stopped at inode `ino` at its own limit `why`,
 * when no fault came first; -EIO, which ends the walk. */
static int check_limit(struct ext2_check *c, uint32_t ino, const char *why)
{
    if (!c->fault) {
        c->limit = 1;
    }
    return check_fault(c, ino, why);
}

/** Whether inode `ino` is in the set of reached inodes. */
static int seen_test(const struct ext2_check *c, uint32_t ino)
{
    return (c->seen[(ino - 1) >> 3] >> ((ino - 1) & 7)) & 1;
}

/** Put inode `ino` in the set of reached inodes. */
static void seen_set(struct ext2_check *c, uint32_t ino)
{
    c->seen[(ino - 1) >> 3] |= (uint8_t)(1u << ((ino - 1) & 7));
}

/** Mark block `b`, which inode `ino` names: 0, or -EIO with the fault
 * recorded for a block out of range or one marked already, a structural
 * block or one another pointer names. */
static int check_mark(struct ext2_check *c, uint32_t ino, uint32_t b)
{
    const struct ext2_mount *em = c->em;

    if (b == 0 || b < em->first_data || b >= em->blocks) {
        return check_fault(c, ino, "a block out of range");
    }
    if (fat_walk_test_set(b)) {
        return check_fault(c, ino, "a block claimed twice");
    }
    return 0;
}

/**
 * Mark pointer block `b` of inode `ino`, at `level`, and what it names,
 * `index` being the data index its first entry stands for; `*count` gains
 * every block, data and pointer, and `*last` is the highest data index
 * named. A pointer block for an index at or past `limit`, the blocks the
 * size covers, is a fault. 0, or -EIO with the fault recorded or for a
 * failed read.
 */
static int check_level(struct ext2_check *c, uint32_t ino, uint32_t b, int level, uint32_t index,
                       uint32_t limit, uint32_t *count, uint32_t *last)
{
    struct ext2_mount *em = c->em;
    uint32_t per = em->block_size / 4;
    uint32_t span = 1, j;
    int i, rc;

    if (index >= limit) {
        return check_fault(c, ino, "a pointer block past the size");
    }
    rc = check_mark(c, ino, b);
    if (rc != 0) {
        return rc;
    }
    (*count)++;
    for (i = 1; i < level; i++) {
        span *= per;
    }
    for (j = 0; j < per; j++) {
        uint8_t raw[4];
        uint32_t p;

        rc = ext2_read_bytes(em, (uint64_t)b * em->block_size + j * 4, raw, 4);
        if (rc != 0) {
            return rc;
        }
        p = ext2_le32(raw);
        if (p == 0) {
            continue;
        }
        if (level == 1) {
            rc = check_mark(c, ino, p);
            if (rc != 0) {
                return rc;
            }
            (*count)++;
            *last = index + j;
        } else {
            rc = check_level(c, ino, p, level - 1, index + j * span, limit, count, last);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

/**
 * Mark the extended-attribute block inode `n` names, once however many
 * inodes share it. Its header must have the magic number and a length of
 * one block. A block whose reference count is 1 or less is marked as any
 * other block is; a shared one is marked and entered in the table by its
 * first inode, and each later inode counts toward the reference count
 * there, more inodes than the count being a fault. 0, or -EIO with the
 * fault recorded or for a failed read.
 */
static int check_attr(struct ext2_check *c, const struct ext2_node *n)
{
    struct ext2_mount *em = c->em;
    uint32_t b = n->file_acl, refs, i;
    uint8_t raw[12];
    int rc;

    if (b < em->first_data || b >= em->blocks) {
        return check_fault(c, n->ino, "a block out of range");
    }
    rc = ext2_read_bytes(em, (uint64_t)b * em->block_size, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }
    if (ext2_le32(raw) != EXT2_XATTR_MAGIC || ext2_le32(raw + 8) != 1) {
        return check_fault(c, n->ino, "an attribute block of the wrong form");
    }
    refs = ext2_le32(raw + 4);
    if (refs <= 1) {
        return check_mark(c, n->ino, b);
    }
    for (i = 0; i < c->attrs; i++) {
        if (c->attr[i].block == b) {
            if (++c->attr[i].found > c->attr[i].expect) {
                return check_fault(c, n->ino, "an attribute block named past its count");
            }
            return 0;
        }
    }
    if (c->attrs >= EXT2_CHECK_ATTRS) {
        return check_limit(c, n->ino, "more shared attribute blocks than the check's table");
    }
    rc = check_mark(c, n->ino, b);
    if (rc != 0) {
        return rc;
    }
    c->attr[c->attrs].block = b;
    c->attr[c->attrs].expect = refs;
    c->attr[c->attrs].found = 1;
    c->attrs++;
    return 0;
}

/**
 * Mark every block `n` names and check them against its size and its
 * `i_blocks`, which counts the extended-attribute block too (`check_attr`).
 * A fast symbolic link, a device, a pipe and a socket keep no other;
 * `as_data` treats the inode as a file whatever its mode, for the reserved
 * inodes. 0, or -EIO with the fault recorded or for a failed read.
 */
static int check_blocks(struct ext2_check *c, const struct ext2_node *n, int as_data)
{
    const struct ext2_mount *em = c->em;
    uint32_t type = n->mode & S_IFMT_;
    uint32_t per = em->block_size / 4;
    uint32_t attr = n->file_acl ? em->block_size / 512 : 0;
    uint32_t count = 0, last = 0, limit, i;
    int named = 0, rc;

    limit = (uint32_t)((n->size + em->block_size - 1) >> em->block_shift);
    if (n->file_acl != 0) {
        rc = check_attr(c, n);
        if (rc != 0) {
            return rc;
        }
    }
    if (!as_data && type != S_IFREG_ && type != S_IFDIR_ &&
        (type != S_IFLNK_ || ext2_fast_link(em, n))) {
        if (n->blocks != attr) {
            return check_fault(c, n->ino, "blocks on an inode that keeps none");
        }
        return 0;
    }
    for (i = 0; i < EXT2_NDIR; i++) {
        if (n->block[i] == 0) {
            continue;
        }
        rc = check_mark(c, n->ino, n->block[i]);
        if (rc != 0) {
            return rc;
        }
        count++;
        last = i;
        named = 1;
    }
    for (i = 0; i < 3; i++) {
        uint32_t base = EXT2_NDIR + (i >= 1 ? per : 0) + (i >= 2 ? per * per : 0);

        if (n->block[EXT2_NDIR + i] == 0) {
            continue;
        }
        rc =
            check_level(c, n->ino, n->block[EXT2_NDIR + i], (int)i + 1, base, limit, &count, &last);
        if (rc != 0) {
            return rc;
        }
        named = 1;
    }
    if (n->blocks != count * (em->block_size / 512) + attr) {
        return check_fault(c, n->ino, "i_blocks that does not match its pointers");
    }
    if (named && last >= limit) {
        return check_fault(c, n->ino, "a block past the size");
    }
    return 0;
}

static int check_dir(struct ext2_check *c, uint32_t ino, uint32_t parent, int depth);

/** Find the exact inode's link counts in the check's index. */
static struct ext2_link *check_link_find(struct ext2_check *c, uint32_t ino)
{
    struct ext2_link *link;

    for (link = c->link[ino % EXT2_CHECK_LINK_BUCKETS]; link; link = link->next) {
        if (link->ino == ino) {
            return link;
        }
    }
    return 0;
}

/** Remember a linked file once; no fixed number of such files is corrupt. */
static int check_link_add(struct ext2_check *c, const struct ext2_node *n)
{
    uint32_t bucket = n->ino % EXT2_CHECK_LINK_BUCKETS;
    struct ext2_link *link = kzalloc(sizeof(*link));

    if (!link) {
        c->link_nomem = 1;
        return -ENOMEM;
    }
    link->ino = n->ino;
    link->expect = n->nlink;
    link->found = 1;
    link->next = c->link[bucket];
    c->link[bucket] = link;
    return 0;
}

/** Require every remembered inode's expected count to match all its names. */
static int check_link_counts(struct ext2_check *c)
{
    uint32_t i;
    struct ext2_link *link;

    for (i = 0; i < EXT2_CHECK_LINK_BUCKETS; i++) {
        for (link = c->link[i]; link; link = link->next) {
            if (link->expect != link->found) {
                return check_fault(c, link->ino, "a link count that does not match its names");
            }
        }
    }
    return 0;
}

/**
 * Check the inode `ino` an entry of directory `dir` names, with the entry's
 * type byte `ft`, at `depth`: a directory is walked once, counted in
 * `*subdirs`, and a second reach is a second parent; a file's blocks are
 * marked once, and a second name counts toward its link count in the
 * table. The type byte, where the volume keeps one, must name the inode's
 * kind, since `ext2_readdir` reports the type from it. 0, or -EIO with the
 * fault recorded or for a failed read.
 */
static int check_child(struct ext2_check *c, uint32_t ino, uint32_t dir, uint32_t ft, int depth,
                       uint32_t *subdirs)
{
    struct ext2_mount *em = c->em;
    struct ext2_node *n;
    uint32_t type;
    struct ext2_link *link;
    int rc;

    if (ino > em->inodes) {
        return check_fault(c, dir, "an entry naming an inode past the count");
    }
    if (depth >= EXT2_CHECK_DEPTH) {
        return check_limit(c, ino, "a directory deeper than the check walks");
    }
    n = &c->node[depth];
    rc = ext2_inode_check_load(em, ino, n);
    if (rc != 0) {
        return rc;
    }
    type = n->mode & S_IFMT_;
    if ((em->incompat & EXT2_INCOMPAT_FILETYPE) && ft != 0 &&
        (ft > 7 || ft != ext2_ftype_of(n->mode))) {
        return check_fault(c, ino, "an entry type that does not match its inode");
    }
    if (type == S_IFDIR_) {
        if (seen_test(c, ino)) {
            return check_fault(c, ino, "a directory with two parents");
        }
        seen_set(c, ino);
        (*subdirs)++;
        return check_dir(c, ino, dir, depth);
    }
    if (seen_test(c, ino)) {
        link = check_link_find(c, ino);
        if (link) {
            if (link->found >= link->expect) {
                return check_fault(c, ino, "a link count that does not match its names");
            }
            link->found++;
            return 0;
        }
        return check_fault(c, ino, "named twice with one link");
    }
    seen_set(c, ino);
    if (type == 0) {
        return check_fault(c, ino, "an entry naming a free inode");
    }
    rc = check_blocks(c, n, 0);
    if (rc != 0) {
        return rc;
    }
    return n->nlink != 1 ? check_link_add(c, n) : 0;
}

/**
 * Walk directory `ino`, loaded at `c->node[depth]`, whose parent is
 * `parent`: its blocks, every entry's form, `.` and `..` in their places,
 * each other entry's inode, and its link count against two plus its
 * subdirectories. 0, or -EIO with the fault recorded or for a failed read.
 */
static int check_dir(struct ext2_check *c, uint32_t ino, uint32_t parent, int depth)
{
    struct ext2_mount *em = c->em;
    const struct ext2_node *n = &c->node[depth];
    uint32_t nblocks, i, subdirs = 0, entry = 0;
    int rc;

    rc = check_blocks(c, n, 0);
    if (rc != 0) {
        return rc;
    }
    if (n->size == 0 || (n->size & (em->block_size - 1)) != 0 || n->size > 0xFFFFFFFFu) {
        return check_fault(c, ino, "a directory size that is not whole blocks");
    }
    nblocks = (uint32_t)(n->size >> em->block_shift);
    for (i = 0; i < nblocks; i++) {
        uint32_t b, off = 0;

        rc = ext2_block_at(em, n, i, &b);
        if (rc != 0) {
            return rc;
        }
        if (b == 0) {
            return check_fault(c, ino, "a hole in a directory");
        }
        while (off < em->block_size) {
            uint8_t raw[10];
            uint64_t at = (uint64_t)b * em->block_size + off;
            uint32_t eino, rec, nlen;

            rc = ext2_read_bytes(em, at, raw, 8);
            if (rc != 0) {
                return rc;
            }
            eino = ext2_le32(raw);
            rec = ext2_le16(raw + 4);
            nlen = raw[6];
            if (rec < 8 || (rec & 3) != 0 || off + rec > em->block_size || nlen + 8 > rec) {
                return check_fault(c, ino, "an entry of the wrong form");
            }
            if (entry < 2) {
                rc = ext2_read_bytes(em, at + 8, raw + 8, nlen < 2 ? nlen : 2);
                if (rc != 0) {
                    return rc;
                }
            }
            if (entry == 0) {
                if (eino != ino || nlen != 1 || raw[8] != '.') {
                    return check_fault(c, ino, "a first entry that is not .");
                }
            } else if (entry == 1) {
                if (eino != parent || nlen != 2 || raw[8] != '.' || raw[9] != '.') {
                    return check_fault(c, ino, "a second entry that is not the parent");
                }
            } else if (eino != 0) {
                if (nlen == 0) {
                    return check_fault(c, ino, "an entry with an empty name");
                }
                rc = check_child(c, eino, ino, raw[7], depth + 1, &subdirs);
                if (rc != 0) {
                    return rc;
                }
            }
            entry++;
            off += rec;
        }
    }
    if (entry < 2) {
        return check_fault(c, ino, "a directory without . and ..");
    }
    if (n->nlink != 2 + subdirs) {
        return check_fault(c, ino, "a directory link count that does not match its entries");
    }
    return 0;
}

/**
 * The inode scan and the bitmap compare, group by group. An orphan is an
 * inode at or past `s_first_ino` the bitmap calls used that the walk did
 * not reach, its blocks marked so that nothing else may claim them; one
 * whose link count is zero is a deleted inode whose bit was not yet
 * cleared, counted with them for `e2fsck` to clear, its pointers not read,
 * since they mean nothing, as `e2fsck` reads such an inode. A used block the
 * walk did not mark is lost. The free counts are recounted into the
 * descriptors in memory and into the mount's totals. 0, or -EIO with the
 * fault recorded or for a failed read.
 */
static int check_bitmaps(struct ext2_check *c)
{
    struct ext2_mount *em = c->em;
    uint32_t g, i;
    int rc;

    em->free_inodes = 0;
    em->free_blocks = 0;
    for (g = 0; g < em->groups; g++) {
        uint8_t *gd = em->gd + g * 32;
        uint32_t free = 0;

        rc = ext2_read_bytes(em, (uint64_t)ext2_le32(gd + 4) * em->block_size, c->bitmap,
                             em->block_size);
        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < em->ipg; i++) {
            uint32_t ino = g * em->ipg + i + 1;
            int used = (c->bitmap[i >> 3] >> (i & 7)) & 1;

            if (ino > em->inodes) {
                break;
            }
            if (!used) {
                free++;
                if (seen_test(c, ino)) {
                    return check_fault(c, ino, "a reached inode the bitmap calls free");
                }
                continue;
            }
            if (seen_test(c, ino) || ino < em->first_ino) {
                continue;
            }
            rc = ext2_inode_check_load(em, ino, &c->node[0]);
            if (rc != 0) {
                return rc;
            }
            c->orphans++;
            if (c->node[0].nlink == 0) {
                continue;
            }
            rc = check_blocks(c, &c->node[0], 0);
            if (rc != 0) {
                return rc;
            }
        }
        LE16_PUT(gd + 14, free);
        em->free_inodes += free;
    }
    for (g = 0; g < em->groups; g++) {
        uint8_t *gd = em->gd + g * 32;
        uint32_t lo = em->first_data + g * em->bpg;
        uint32_t free = 0;

        rc = ext2_read_bytes(em, (uint64_t)ext2_le32(gd + 0) * em->block_size, c->bitmap,
                             em->block_size);
        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < em->bpg && lo + i < em->blocks; i++) {
            int used = (c->bitmap[i >> 3] >> (i & 7)) & 1;
            int marked = fat_walk_marked(lo + i);

            if (!used) {
                free++;
                if (marked) {
                    return check_fault(c, 0, "a used block the bitmap calls free");
                }
            } else if (!marked) {
                c->lost++;
            }
        }
        LE16_PUT(gd + 12, free);
        em->free_blocks += free;
    }
    return 0;
}

/**
 * Log what a check without a fault leaves for e2fsck, setting the error bit
 * when it leaves anything: the shared attribute blocks whose count is above
 * the inodes found naming them, then the lost blocks and orphans; or that
 * the volume is clean.
 */
static void check_report(struct ext2_check *c)
{
    uint32_t i;

    for (i = 0; i < c->attrs; i++) {
        if (c->attr[i].found < c->attr[i].expect) {
            c->stale++;
        }
    }
    if (c->stale != 0) {
        klog(KLOG_WARN, "ext2",
             "check: %d attribute blocks with counts above their inodes, kept for e2fsck",
             (int)c->stale);
        c->em->state |= EXT2_ERROR_FS;
    }
    if (c->lost != 0 || c->orphans != 0) {
        klog(KLOG_WARN, "ext2", "check: %d lost blocks, %d orphan inodes, kept for e2fsck",
             (int)c->lost, (int)c->orphans);
        c->em->state |= EXT2_ERROR_FS;
    } else if (c->stale == 0) {
        klog(KLOG_INFO, "ext2", "check: clean");
    }
}

/**
 * End a check that met its first fault or one of its own limits, `c->fault`:
 * the free counts put back to `free_blocks` and `free_inodes` and the
 * descriptors read again, since the recount may have stopped part way; the
 * fault logged, and the error bit set for a fault of the volume or
 * `em->check_owed` for a limit of the check's, as `check_volume` says.
 * 0, or the read's -errno.
 */
static int check_stopped(struct ext2_check *c, uint64_t free_blocks, uint64_t free_inodes)
{
    struct ext2_mount *em = c->em;
    int rc;

    em->free_blocks = free_blocks;
    em->free_inodes = free_inodes;
    rc = ext2_read_bytes(em, (uint64_t)(em->first_data + 1) * em->block_size, em->gd,
                         em->groups * 32);
    if (rc != 0) {
        return rc;
    }
    if (c->fault_ino != 0) {
        klog(KLOG_WARN, "ext2", "check: inode %d: %s", (int)c->fault_ino, c->fault);
    } else {
        klog(KLOG_WARN, "ext2", "check: %s", c->fault);
    }
    if (c->limit) {
        em->check_owed = 1;
        klog(KLOG_WARN, "ext2", "mounting unchecked, running e2fsck is recommended");
    } else {
        em->state |= EXT2_ERROR_FS;
        klog(KLOG_WARN, "ext2", "mounting with errors, running e2fsck is recommended");
    }
    return 0;
}

/**
 * Check a volume whose valid bit is clear or whose error bit is set, at a
 * writable mount or remount, before it is written, as the section above
 * says: the structural blocks and the reserved inodes below `s_first_ino`
 * marked first, then the tree walked from the root, the link counts
 * compared, and the bitmaps.
 *
 * Returns 0 and leaves the volume to be mounted writable, as Linux's ext2
 * mounts any volume it supports whatever its state (`ext2_setup_super`):
 *
 * - a check that ends writes the recounted free counts to the descriptors
 *   and the superblock, with the error bit set when blocks were lost,
 *   inodes orphaned or attribute blocks left counting inodes that no longer
 *   name them;
 * - a fault is logged as `ext2: check: inode N: WHY` and sets the error
 *   bit in memory, which the mount's write of the superblock puts on the
 *   disk, with the counts as the disk has them, since the recount may have
 *   stopped part way;
 * - a check stopped at one of its own limits is logged by name and sets
 *   `em->check_owed`, with the counts as the disk has them and the state
 *   as it is, so that no unmount marks the volume valid and the next
 *   writable mount checks it again.
 *
 * The last two log `ext2: mounting with errors, running e2fsck is
 * recommended` or `ext2: mounting unchecked, running e2fsck is
 * recommended`, Linux's warnings. -errno when the device failed.
 */
static int check_volume(struct ext2_mount *em)
{
    struct ext2_check *c;
    uint64_t free_blocks = em->free_blocks, free_inodes = em->free_inodes;
    uint32_t g, ino;
    int rc;

    em->check_owed = 0;
    c = check_state(em);
    if (!c) {
        klog(KLOG_WARN, "ext2", "check: no memory for the %d KiB of workspace the volume needs",
             (int)((fat_walk_bytes(em->blocks) + (em->inodes + 7) / 8 + sizeof(*c)) / 1024));
        em->check_owed = 1;
        klog(KLOG_WARN, "ext2", "mounting unchecked, running e2fsck is recommended");
        return 0;
    }
    for (g = 0; g < em->groups; g++) {
        const uint8_t *gd = em->gd + g * 32;
        uint32_t lo = em->first_data + g * em->bpg, b;

        if (ext2_has_super(em, g)) {
            for (b = lo; b <= lo + ext2_gd_blocks(em); b++) {
                fat_walk_test_set(b);
            }
        }
        fat_walk_test_set(ext2_le32(gd + 0));
        fat_walk_test_set(ext2_le32(gd + 4));
        for (b = 0; b < ext2_inode_table_blocks(em); b++) {
            fat_walk_test_set(ext2_le32(gd + 8) + b);
        }
    }
    rc = 0;
    for (ino = 1; ino < em->first_ino && rc == 0; ino++) {
        seen_set(c, ino);
        if (ino == EXT2_ROOT_INO) {
            continue;
        }
        rc = ext2_inode_check_load(em, ino, &c->node[0]);
        if (rc == 0 && c->node[0].blocks != 0) {
            rc = check_blocks(c, &c->node[0], 1);
        }
    }
    if (rc == 0) {
        seen_set(c, EXT2_ROOT_INO); /* not below first_ino on every volume */
        rc = ext2_inode_check_load(em, EXT2_ROOT_INO, &c->node[0]);
    }
    if (rc == 0) {
        rc = check_dir(c, EXT2_ROOT_INO, EXT2_ROOT_INO, 0);
    }
    if (rc == 0) {
        rc = check_link_counts(c);
    }
    if (rc == 0) {
        rc = check_bitmaps(c);
    }
    fat_walk_release();
    if (rc == -ENOMEM && c->link_nomem && !c->fault) {
        (void)check_limit(c, 0, "no memory to count the hard links");
    }
    if (rc != 0 && !c->fault) {
        check_state_free(c);
        return rc;
    }
    if (c->fault) {
        rc = check_stopped(c, free_blocks, free_inodes);
        check_state_free(c);
        return rc;
    }
    for (g = 0; g < em->groups; g++) {
        ext2_group_lock(em, g);
        rc = ext2_gd_store(em, g);
        ext2_group_unlock(em, g);
        if (rc != 0) {
            check_state_free(c);
            return rc;
        }
    }
    check_report(c);
    check_state_free(c);
    return ext2_sb_store(em);
}

/**
 * `check_volume` with `em->opening` set, the one time a mount that is not
 * yet writable may write (`ext2_write_owned`): the recounted descriptors
 * and superblock of a check that ends. The flag is given back as the
 * caller had it, since a remount writable keeps it set for the writes that
 * follow the check. Returns what `check_volume` does.
 */
int ext2_check_volume(struct ext2_mount *em)
{
    int was = em->opening;
    int rc;

    em->opening = 1;
    rc = check_volume(em);
    em->opening = was;
    return rc;
}
