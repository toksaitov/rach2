/*
 * ext2_check.c: the check of an ext2 volume that was not unmounted cleanly, at
 * its writable mount, which repairs free counts, the bits of blocks a file
 * names, the entries a stopped create leaves and link counts, as e2fsck -p
 * does.
 */

#include "ext2_internal.h"

/* ---- the mount-time check ---- */

/*
 * A volume whose valid bit is clear was not unmounted, and one whose error
 * bit is set was found faulty by an earlier mount. Either is checked
 * before it is written, under the walk lock and the shared bitmap the FAT
 * drivers use: the structural blocks are seeded into the bitmap, the tree
 * is walked from the root and every block every reached inode names is
 * marked, its extended-attribute block among them, every inode the walk
 * did not reach is read, the live ones and those the inode bitmap calls
 * used being orphans, the entries of orphan directories are walked as the
 * tree's are, and the bitmaps are compared with the marks. A block a file
 * names whose bit is clear, which a stop leaves when a pointer block
 * reached the disk before the bitmap, has its bit set, so that the
 * allocator cannot give it to another file while the pointer names it, and
 * so has a live orphan inode whose bit is clear, since the allocator takes
 * the first clear bit without reading the inode. An entry naming an unused
 * inode, which a stop leaves when a new name reached the disk before its
 * inode, is cleared, and an entry whose type byte disagrees with its live
 * inode's mode gets the inode's type, as `e2fsck -p` repairs both in its
 * pass 2. A link count that disagrees with the names the walk found, which
 * a stop leaves when a name reached the disk before or after the count of
 * its inode or of a new directory's parent, is set to those names, as
 * `e2fsck -p` sets it in its pass 4, unless it is 0 or its inode is an
 * orphan directory, whose count is judged once every orphan tree is walked:
 * an orphan directory no entry names that holds only `.` and `..`, which an
 * rmdir or a mkdir cut off on a mount that writes back leaves, with holes
 * where the free of its blocks was cut off too, has its
 * count set to 0, whatever it was, so that it is a removed inode e2fsck
 * clears, unless the `..` of a kept orphan directory names it, and any
 * other orphan directory keeps its count, which must match its
 * subdirectories. All are written only once the check ends without a fault,
 * and synced to the medium before the mount takes a write. Nothing else is
 * repaired.
 * Lost blocks, orphans, regular files and directories whose size or `i_blocks`
 * disagrees with their pointers, which a stop leaves when the pointers reached
 * the disk before the inode, and shared attribute blocks whose reference count
 * is above the inodes that name them are counted and kept, held out of the
 * allocator by the bits and counts they have, and the error bit is set so that
 * the volume is not marked clean again before `e2fsck` has run; the volume
 * mounts writable, as Linux's ext2 mounts a volume whose error bit is set with
 * a warning (`ext2_setup_super`). The group descriptors' free counts are
 * recomputed from the bitmaps rather than taken from the disk, since a crash
 * leaves them stale.
 *
 * A fault stops the check at the first one met: a block claimed twice or out of
 * range, an inode neither a regular file nor a directory whose pointers do not
 * match its size or its `i_blocks`, an attribute block of the wrong form or
 * named by more inodes than its count, a directory with two parents or without
 * its parent, a hole in a directory but an empty orphan one, an entry naming an
 * inode in a directory's block past its size, a kept orphan directory whose
 * `..` names anything but the root or a directory of an orphan tree, or whose
 * link count does not match its subdirectories, an empty one whose `..` names
 * no inode a volume can have, an entry of the wrong form or naming a reserved
 * inode, a link count of 0 that an entry names, a block of the volume's tables
 * or a reached inode the bitmap calls free. So does one of
 * the check's own limits, below. A check that stops has not compared the
 * bitmaps with the blocks files name, and the faults it stops at are the
 * ones under which a free could take a block or an inode something else
 * still names: a block claimed twice, an attribute block's count below its
 * sharers, a named inode with no link, an orphan directory's `..` naming
 * an inode that an unlink, an rmdir or a rename could free. So the volume
 * mounts read-only, and nothing is allocated, freed or reclaimed on it
 * before `e2fsck` has run or a later writable remount's check ends; a fault
 * sets the error bit on the disk, as Linux's `ext2_error` writes it before
 * it makes a mount read-only.
 *
 * The walk keeps its state on the heap rather than on the stack: the node
 * of each directory level, the hard links, the live orphans and the shared
 * attribute blocks met with their expected and found counts, the
 * reached-inode set, one bit per inode of the volume, and one bitmap
 * block; the block bitmap is the one the FAT walks share. A dirty volume
 * whose set or bitmap the free memory cannot hold, a directory deeper than
 * the levels, more shared attribute blocks than its table holds, or hard
 * links and orphans the heap cannot record stop the check, which says
 * which: none is a fault of the volume, whose state, which called for the
 * check, is left as it is. Hard-link and orphan records are allocated as
 * files are met.
 */
#define EXT2_CHECK_DEPTH        32  /* the directory levels the walk holds */
#define EXT2_CHECK_LINK_BUCKETS 512 /* the index, not a limit on linked files */
#define EXT2_CHECK_ATTRS        512 /* the shared attribute blocks it counts */

/* An entry or a link count the check repairs once it ends (`check_fixes`):
 * the byte on the device of the entry, or of the inode's link count; the
 * inode the entry names, or whose count it is, and the entry's directory;
 * the type byte to write, or 0 to clear the entry's inode number; and for a
 * count, `was` nonzero, the count to write (`links`) and the one found. */
struct ext2_fix {
    struct ext2_fix *next;
    uint64_t at;
    uint32_t ino, dir;
    uint32_t ft;
    uint32_t links, was;
};

/* An extended-attribute block whose header counts more than one reference:
 * the block, that count, and the inodes the check has found naming it. */
struct ext2_attr {
    uint32_t block;
    uint32_t expect, found;
};

/*
 * The check's state: the first fault and its inode; the lost blocks, the
 * orphans, the blocks a file names whose bits it set (`marked`), the live
 * orphan inodes whose bits it set (`iset`), the regular files whose size
 * or `i_blocks` disagrees with their pointers (`sized`), the shared
 * attribute blocks counting more references than the inodes that name
 * them (`stale`), the chained records of `link` and the `attrs` entries of
 * `attr`; the entries to repair, in the order met, with their count
 * (`fixes`); the byte on the device of the entry `check_child` judges
 * (`at`); the orphan directory whose tree is being walked (`root`); the
 * inode of each directory level being walked; the set of reached inodes;
 * and one bitmap block.
 */
struct ext2_check {
    struct ext2_mount *em;
    const char *fault;
    uint32_t fault_ino;
    int limit; /* `fault` is a limit of the check's, not of the volume */
    uint32_t lost, orphans, marked, iset, sized, stale, attrs, fixes;
    struct ext2_fix *fix, **fix_tail;
    uint64_t at;
    uint32_t root;
    int keep; /* the inode being marked keeps a mismatch for e2fsck */
    int past; /* and `check_level` met a pointer block past its size */
    int dir;  /* the inode being marked is a directory, its blocks past the size read */
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
    c->fix_tail = &c->fix;
    return c;
}

/** Free the check's state, inode set and entry repairs; the walk bitmap is
 * given back apart, by `fat_walk_release`. */
static void check_state_free(struct ext2_check *c)
{
    uint32_t i;
    struct ext2_link *link, *next;
    struct ext2_fix *fix, *fnext;

    for (fix = c->fix; fix; fix = fnext) {
        fnext = fix->next;
        kfree(fix);
    }

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
 * Require block `b`, which directory `ino` names past its size, to hold no
 * entry: records of the form `check_dir` requires, each of inode 0, as a
 * growth writes a new block before its first name, or zeros throughout, as
 * a stop leaves a new block whose zeros reached the disk before its first
 * record. The walk reads no entry past the size and counts no name there,
 * and e2fsck, which takes the size up to the blocks, would read one. 0, or
 * -EIO with the fault recorded or for a failed read.
 */
static int check_past(struct ext2_check *c, uint32_t ino, uint32_t b)
{
    struct ext2_mount *em = c->em;
    uint64_t at = (uint64_t)b * em->block_size;
    uint32_t off, rec, i;
    uint8_t raw[64];
    int rc;

    for (off = 0; off < em->block_size; off += rec) {
        rc = ext2_read_bytes(em, READ_QUIET, 0, at + off, raw, 8);
        if (rc != 0) {
            return rc;
        }
        rec = ext2_le16(raw + 4);
        if (off == 0 && rec == 0 && ext2_le32(raw) == 0) {
            break;
        }
        if (rec < 8 || (rec & 3) != 0 || off + rec > em->block_size || raw[6] + 8u > rec ||
            ext2_le32(raw) != 0) {
            return check_fault(c, ino, "an entry past a directory's size");
        }
    }
    for (off = 0; off < em->block_size && rec == 0; off += sizeof(raw)) {
        rc = ext2_read_bytes(em, READ_QUIET, 0, at + off, raw, sizeof(raw));
        for (i = 0; rc == 0 && i < sizeof(raw); i++) {
            if (raw[i] != 0) {
                return check_fault(c, ino, "an entry past a directory's size");
            }
        }
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/**
 * Mark data block `b` of inode `ino`, at data index `index`, and, for a
 * directory (`c->dir`) at or past `limit`, the blocks its size covers,
 * require it empty (`check_past`). 0, or -EIO with the fault recorded or
 * for a failed read.
 */
static int check_data(struct ext2_check *c, uint32_t ino, uint32_t b, uint32_t index,
                      uint32_t limit)
{
    int rc = check_mark(c, ino, b);

    if (rc == 0 && c->dir && index >= limit) {
        rc = check_past(c, ino, b);
    }
    return rc;
}

/**
 * Mark pointer block `b` of inode `ino`, at `level`, and what it names,
 * `index` being the data index its first entry stands for; `*count` gains
 * every block, data and pointer, and `*last` is the highest data index
 * named. A pointer block for an index at or past `limit`, the blocks the
 * size covers, sets `c->past` and is marked with all it names when
 * `c->keep` is set, for a regular file or a directory, and is a fault
 * otherwise. 0, or -EIO with the fault
 * recorded or for a failed read.
 */
static int check_level(struct ext2_check *c, uint32_t ino, uint32_t b, int level, uint32_t index,
                       uint32_t limit, uint32_t *count, uint32_t *last)
{
    struct ext2_mount *em = c->em;
    uint32_t per = em->block_size / 4;
    uint32_t span = 1, j;
    int i, rc;

    if (index >= limit) {
        if (!c->keep) {
            return check_fault(c, ino, "a pointer block past the size");
        }
        c->past = 1;
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

        rc = ext2_read_bytes(em, READ_QUIET, 0, (uint64_t)b * em->block_size + j * 4, raw, 4);
        if (rc != 0) {
            return rc;
        }
        p = ext2_le32(raw);
        if (p == 0) {
            continue;
        }
        if (level == 1) {
            rc = check_data(c, ino, p, index + j, limit);
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
    rc = ext2_read_bytes(em, READ_QUIET, 0, (uint64_t)b * em->block_size, raw, sizeof(raw));
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
 * Judge inode `n` once its blocks are marked: `blocks_wrong` when its
 * `i_blocks` does not match them, `past` when a data block is past its
 * size, `c->past` when a pointer block is. A regular file or a directory
 * (`c->keep`) with any of them is counted in `c->sized`; any other inode
 * with one is a fault. 0, or -EIO with the fault recorded.
 */
static int check_size(struct ext2_check *c, const struct ext2_node *n, int blocks_wrong, int past)
{
    if (c->keep) {
        if (blocks_wrong || past || c->past) {
            c->sized++;
        }
        return 0;
    }
    if (blocks_wrong) {
        return check_fault(c, n->ino, "i_blocks that does not match its pointers");
    }
    if (past) {
        return check_fault(c, n->ino, "a block past the size");
    }
    return 0;
}

/**
 * Mark every block `n` names and check them against its size and its
 * `i_blocks`, which counts the extended-attribute block too (`check_attr`).
 * A fast symbolic link, a device, a pipe and a socket keep no other;
 * `as_data` treats the inode as a file whatever its mode, for the reserved
 * inodes. A regular file or a directory whose blocks pass its size or disagree
 * with its `i_blocks` is counted in `c->sized` and kept for `e2fsck`, every
 * block it names marked, since a stop leaves its pointer blocks on the disk
 * before its inode (ext2.Readme.md, The order of writing back), a directory's
 * when it grows through a pointer block it has or frees its blocks at an rmdir;
 * a directory's entries past its size are not read, and its next growth takes
 * the block mapped there again. In any other inode it is a fault. 0, or -EIO
 * with the fault recorded or for a failed read.
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
    c->keep = !as_data && (type == S_IFREG_ || type == S_IFDIR_);
    c->dir = !as_data && type == S_IFDIR_;
    c->past = 0;
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
        rc = check_data(c, n->ino, n->block[i], i, limit);
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
    return check_size(c, n, n->blocks != count * (em->block_size / 512) + attr,
                      named && last >= limit);
}

static int check_dir(struct ext2_check *c, uint32_t ino, uint32_t parent, int depth);

/**
 * Append a zeroed repair of byte `at` for inode `ino` to the check's list,
 * written once the check ends (`check_fixes`). The record, or NULL with the
 * check's limit recorded when the heap cannot hold it.
 */
static struct ext2_fix *check_fix_new(struct ext2_check *c, uint64_t at, uint32_t ino)
{
    struct ext2_fix *fix = kzalloc(sizeof(*fix));

    if (!fix) {
        (void)check_limit(c, ino, "no memory to record the entries it repairs");
        return 0;
    }
    fix->at = at;
    fix->ino = ino;
    *c->fix_tail = fix;
    c->fix_tail = &fix->next;
    c->fixes++;
    return fix;
}

/**
 * Record the repair of the entry at byte `at` of directory `dir`, which
 * names inode `ino`: its type byte set to `ft`, or with `ft` 0 its inode
 * number cleared, written once the check ends (`check_fixes`). 0, or -EIO
 * with the check's limit recorded when the heap cannot hold the record.
 */
static int check_fix_add(struct ext2_check *c, uint64_t at, uint32_t ino, uint32_t dir, uint32_t ft)
{
    struct ext2_fix *fix = check_fix_new(c, at, ino);

    if (!fix) {
        return -EIO;
    }
    fix->dir = dir;
    fix->ft = ft;
    return 0;
}

/**
 * Record the repair of inode `ino`'s link count `was` to `links`, the names
 * the walk found, or 0 for an empty orphan directory (`check_orphan_parents`),
 * written once the check ends (`check_fixes`), as `e2fsck -p` sets a count in
 * its pass 4. A `was` of 0, which `e2fsck` reads as a deleted inode, and a
 * `links` past the 16 bits of `i_links_count` are faults. 0, or -EIO with the
 * fault or the check's limit recorded.
 */
static int check_count_add(struct ext2_check *c, uint32_t ino, uint32_t was, uint32_t links)
{
    struct ext2_fix *fix;
    uint64_t at;
    int rc;

    if (was == 0 || links > 0xFFFFu) {
        return check_fault(c, ino, "a link count that does not match its names");
    }
    rc = ext2_slot_at(c->em, READ_QUIET, ino, &at);
    if (rc != 0) {
        return rc;
    }
    fix = check_fix_new(c, at + 26, ino);
    if (!fix) {
        return -EIO;
    }
    fix->links = links;
    fix->was = was;
    return 0;
}

/**
 * Whether inode `n`, which an entry names and the walk has not reached, is
 * unused, into `*unused`: with no mode, or with no link and either a
 * deletion time or its bit clear in the inode bitmap, as `e2fsck` counts an
 * inode unused. A stop leaves an entry naming one when the entry reached the
 * disk before its new inode. 0, or the -errno of a failed read.
 */
static int check_unused(struct ext2_check *c, const struct ext2_node *n, int *unused)
{
    struct ext2_mount *em = c->em;
    uint32_t g = (n->ino - 1) / em->ipg, at = (n->ino - 1) % em->ipg;
    uint8_t raw[4];
    int rc;

    *unused = n->mode == 0;
    if (*unused || n->nlink != 0) {
        return 0;
    }
    rc = ext2_read_bytes(em, READ_QUIET, 0, ext2_inode_offset(em, n->ino) + 20, raw, 4);
    if (rc != 0) {
        return rc;
    }
    if (ext2_le32(raw) != 0) { /* i_dtime */
        *unused = 1;
        return 0;
    }
    rc =
        ext2_read_bytes(em, READ_QUIET, 0,
                        (uint64_t)ext2_le32(em->gd + g * 32 + 4) * em->block_size + at / 8, raw, 1);
    if (rc != 0) {
        return rc;
    }
    *unused = !((raw[0] >> (at & 7)) & 1);
    return 0;
}

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

/** Enter `link` in the check's index. */
static void check_link_put(struct ext2_check *c, struct ext2_link *link)
{
    uint32_t bucket = link->ino % EXT2_CHECK_LINK_BUCKETS;

    link->next = c->link[bucket];
    c->link[bucket] = link;
}

/** Remember a linked file once; no fixed number of such files is corrupt. */
static int check_link_add(struct ext2_check *c, const struct ext2_node *n)
{
    struct ext2_link *link = kzalloc(sizeof(*link));

    if (!link) {
        c->link_nomem = 1;
        return -ENOMEM;
    }
    link->ino = n->ino;
    link->expect = n->nlink;
    link->found = 1;
    check_link_put(c, link);
    return 0;
}

/**
 * Set every remembered inode's count that does not match all its names,
 * those in the entries of orphan directories among them, to those names
 * (`check_count_add`). An orphan no entry named is kept for e2fsck, and an
 * orphan directory an entry named had its count judged by `check_dir`. 0,
 * or -EIO with the fault or the check's limit recorded.
 */
static int check_link_counts(struct ext2_check *c)
{
    uint32_t i;
    struct ext2_link *link;
    int rc;

    for (i = 0; i < EXT2_CHECK_LINK_BUCKETS; i++) {
        for (link = c->link[i]; link; link = link->next) {
            if (!link->orphan && !link->dir && link->expect != link->found) {
                rc = check_count_add(c, link->ino, link->expect, link->found);
                if (rc != 0) {
                    return rc;
                }
            }
        }
    }
    return 0;
}

/** Count orphan `link` as named by an entry: no longer an orphan. */
static void check_adopt(struct ext2_check *c, struct ext2_link *link)
{
    link->orphan = 0;
    c->orphans--;
}

/**
 * Judge the entry at `c->at` of directory `dir`, which names inode `n`,
 * with type byte `ft`, before the inode is walked. An unused inode the walk
 * has not reached (`check_unused`) has its entry cleared. Any other inode
 * whose mode is of no kind is a fault, whatever the entry's type byte and
 * with or without the volume's FILETYPE, since it is live, not unused, and
 * is no kind of file whose blocks the check could judge. A type byte, where
 * the volume keeps one, that does not name the live inode's kind is set to
 * it, since `ext2_readdir` reports the type from it (`check_fix_add`). 1
 * when the entry is cleared and the inode not to be walked, 0 when it is to
 * be walked, or -EIO with the fault or a limit recorded or for a failed
 * read.
 */
static int check_entry(struct ext2_check *c, const struct ext2_node *n, uint32_t dir, uint32_t ft)
{
    struct ext2_mount *em = c->em;
    uint32_t want = ext2_ftype_of(n->mode);
    int rc, unused;

    if (n->ino >= em->first_ino && !seen_test(c, n->ino)) {
        rc = check_unused(c, n, &unused);
        if (rc != 0) {
            return rc;
        }
        if (unused) {
            rc = check_fix_add(c, c->at, n->ino, dir, 0);
            return rc != 0 ? rc : 1;
        }
    }
    if (want == 0) {
        return check_fault(c, n->ino, "an entry naming an inode of no kind");
    }
    if (!(em->incompat & EXT2_INCOMPAT_FILETYPE) || ft == 0 || ft == want) {
        return 0;
    }
    return check_fix_add(c, c->at, n->ino, dir, want);
}

/**
 * Check the inode `ino` the entry at `c->at` of directory `dir` names, with
 * the entry's type byte `ft`, at `depth`, once `check_entry` has judged the
 * entry, which may clear it. A directory is walked once, counted in
 * `*subdirs`, and a second reach is a second parent; a file's blocks are
 * marked once, and a second name counts toward its link count in the
 * table, which `check_link_counts` compares once every name is found. An
 * orphan (`check_orphan`) that an entry of an orphan directory
 * names is counted as named: a file's blocks were marked already, and a
 * directory is walked unless it was walked as an orphan already, in which
 * case its `..` must be `dir` and its count, kept unjudged in its record
 * (`check_dir_count`), is set to two plus its subdirectories when it
 * differs, as `check_dir` sets it when the entry comes first. 1 when
 * `check_entry` cleared the entry, 0, or -EIO with the fault or a limit
 * recorded or for a failed read.
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
    rc = check_entry(c, n, dir, ft);
    if (rc != 0) {
        return rc;
    }
    type = n->mode & S_IFMT_;
    link = c->root != 0 ? check_link_find(c, ino) : 0;
    if (type == S_IFDIR_) {
        if (seen_test(c, ino)) {
            if (!link || !link->orphan || ino == c->root || link->parent != dir) {
                return check_fault(c, ino, "a directory with two parents");
            }
            if (link->holes) {
                return check_fault(c, ino, "a hole in a directory");
            }
            check_adopt(c, link);
            (*subdirs)++;
            if (link->expect != 2 + link->subdirs) {
                return check_count_add(c, ino, link->expect, 2 + link->subdirs);
            }
            return 0;
        }
        if (link && link->orphan) {
            check_adopt(c, link);
        }
        seen_set(c, ino);
        (*subdirs)++;
        return check_dir(c, ino, dir, depth);
    }
    if (seen_test(c, ino)) {
        link = check_link_find(c, ino);
        if (link) {
            if (link->orphan) {
                check_adopt(c, link);
            }
            link->found++;
            return 0;
        }
        if (ino < em->first_ino) {
            return check_fault(c, ino, "an entry naming a reserved inode");
        }
        rc = check_link_add(c, n);
        if (rc == 0) {
            check_link_find(c, ino)->found++;
        }
        return rc;
    }
    seen_set(c, ino);
    rc = check_blocks(c, n, 0);
    if (rc != 0) {
        return rc;
    }
    return n->nlink != 1 ? check_link_add(c, n) : 0;
}

/**
 * Check entry `entry`, 0 or 1, of directory `ino`, read into `raw` with the
 * first two bytes of its name: `.` naming the directory, and `..` naming
 * `parent`, or any inode when `parent` is 0, an orphan directory's, whose
 * `..` is kept in its record and judged once every directory is walked, by
 * `check_child` when an entry names the directory and by
 * `check_orphan_parents` otherwise. 0, or -EIO with the fault recorded.
 */
static int check_dots(struct ext2_check *c, uint32_t ino, uint32_t parent, uint32_t entry,
                      const uint8_t *raw)
{
    uint32_t eino = ext2_le32(raw), nlen = raw[6];
    struct ext2_link *link;

    if (entry == 0) {
        if (eino != ino || nlen != 1 || raw[8] != '.') {
            return check_fault(c, ino, "a first entry that is not .");
        }
        return 0;
    }
    if ((parent != 0 && eino != parent) || nlen != 2 || raw[8] != '.' || raw[9] != '.') {
        return check_fault(c, ino, "a second entry that is not the parent");
    }
    link = parent == 0 ? check_link_find(c, ino) : 0;
    if (link) {
        link->parent = eino;
    }
    return 0;
}

/**
 * Judge the link count of directory `n`, whose parent is `parent`, against
 * two plus its `subdirs` subdirectories: a count that differs is set to that
 * (`check_count_add`). A `parent` of 0 is an orphan directory's, walked as
 * no entry's: its subdirectories, whether it holds anything but `.` and
 * `..` once the check's repairs are made, `others` being the entries it
 * keeps, and whether it has `holes` go in its record, and its count is
 * judged once every orphan tree is walked, by `check_child` when an entry
 * of another names it and by `check_orphan_parents` otherwise, so that the
 * order of the walk changes nothing. 0, or -EIO with the fault or the
 * check's limit recorded or for a failed read.
 */
static int check_dir_count(struct ext2_check *c, const struct ext2_node *n, uint32_t parent,
                           uint32_t subdirs, uint32_t others, uint32_t holes)
{
    struct ext2_link *link;

    if (parent != 0) {
        return n->nlink != 2 + subdirs ? check_count_add(c, n->ino, n->nlink, 2 + subdirs) : 0;
    }
    link = check_link_find(c, n->ino);
    if (link) {
        link->subdirs = subdirs;
        link->empty = others == 0;
        link->holes = holes != 0;
    }
    return 0;
}

/**
 * Walk directory `ino`, loaded at `c->node[depth]`, whose parent is
 * `parent`: its blocks, those past its size empty (`check_past`), every
 * entry's form, `.` and `..` in their places, each other entry's inode, and
 * its link count against two plus its subdirectories (`check_dir_count`).
 * A `parent` of 0 is an orphan directory's, whose `..` is kept in its
 * record and judged later (`check_dots`), and whose holes, which a stop in
 * the free of a removed directory's blocks leaves before its count of 0
 * reached the disk, are counted for `check_orphan_parent`; any other
 * directory's hole is a fault. 0, or -EIO with the fault recorded or for a
 * failed read.
 */
static int check_dir(struct ext2_check *c, uint32_t ino, uint32_t parent, int depth)
{
    struct ext2_mount *em = c->em;
    const struct ext2_node *n = &c->node[depth];
    uint32_t nblocks, i, subdirs = 0, entry = 0, others = 0, holes = 0;
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
        if (b == 0 && parent != 0) {
            return check_fault(c, ino, "a hole in a directory");
        }
        holes += b == 0;
        while (b != 0 && off < em->block_size) {
            uint8_t raw[10];
            uint64_t at = (uint64_t)b * em->block_size + off;
            uint32_t eino, rec, nlen;

            rc = ext2_read_bytes(em, READ_QUIET, 0, at, raw, 8);
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
                rc = ext2_read_bytes(em, READ_QUIET, 0, at + 8, raw + 8, nlen < 2 ? nlen : 2);
                if (rc == 0) {
                    rc = check_dots(c, ino, parent, entry, raw);
                }
                if (rc != 0) {
                    return rc;
                }
            } else if (eino != 0) {
                if (nlen == 0) {
                    return check_fault(c, ino, "an entry with an empty name");
                }
                c->at = at;
                rc = check_child(c, eino, ino, raw[7], depth + 1, &subdirs);
                if (rc < 0) {
                    return rc;
                }
                others += rc == 0;
            }
            entry++;
            off += rec;
        }
    }
    if (entry < 2) {
        return check_fault(c, ino, "a directory without . and ..");
    }
    return check_dir_count(c, n, parent, subdirs, others, holes);
}

/**
 * Compare each group's block bitmap with the marks. A used block nothing
 * marked is lost. A marked block the bitmap calls free is a fault when it
 * holds the volume's tables (`ext2_data_block_ok`), which only a damaged
 * bitmap leaves; one a file names, which a stop leaves when its pointer
 * reached the disk before its bit, has its bit set and is counted in
 * `c->marked`. Without `repair` nothing is counted or written, so that a
 * fault leaves the bitmaps as they were; with it each changed bitmap block
 * is written and the free counts are recounted into the descriptors in
 * memory and into the mount's total. 0, or -EIO with the fault recorded,
 * or the -errno of a failed read or write.
 */
static int check_block_bitmaps(struct ext2_check *c, int repair)
{
    struct ext2_mount *em = c->em;
    uint32_t g, i;
    int rc;

    if (repair) {
        em->free_blocks = 0;
    }
    for (g = 0; g < em->groups; g++) {
        uint8_t *gd = em->gd + g * 32;
        uint64_t at = (uint64_t)ext2_le32(gd + 0) * em->block_size;
        uint32_t lo = em->first_data + g * em->bpg;
        uint32_t free = 0;
        int changed = 0;

        rc = ext2_read_bytes(em, READ_QUIET, 0, at, c->bitmap, em->block_size);
        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < em->bpg && lo + i < em->blocks; i++) {
            int used = (c->bitmap[i >> 3] >> (i & 7)) & 1;
            int marked = fat_walk_marked(lo + i);

            if (!used && marked) {
                if (!ext2_data_block_ok(em, lo + i)) {
                    return check_fault(c, 0, "a used block the bitmap calls free");
                }
                if (repair) {
                    c->bitmap[i >> 3] = (uint8_t)(c->bitmap[i >> 3] | (1u << (i & 7)));
                    c->marked++;
                    changed = 1;
                }
                used = 1;
            }
            if (!used) {
                free++;
            } else if (!marked && repair) {
                c->lost++;
            }
        }
        if (!repair) {
            continue;
        }
        if (changed) {
            ext2_group_lock(em, g);
            rc = ext2_write_bytes(em, at, c->bitmap, em->block_size,
                                  WRITE_THROUGH | WRITE_KIND(READ_BITMAP));
            ext2_group_unlock(em, g);
            if (rc != 0) {
                return rc;
            }
        }
        LE16_PUT(gd + 12, free);
        em->free_blocks += free;
    }
    return 0;
}

/**
 * Record `n`, an inode at or past `s_first_ino` the walk did not reach that
 * is live, with a mode and a link, as an orphan: counted, in the index with
 * its link count and no name found, and with `used` clear its bit to be
 * set (`check_inode_bits`). A file has its blocks marked; a directory's are
 * marked when its entries are walked (`check_orphan_dirs`). 0, -ENOMEM
 * with `c->link_nomem` set, or what `check_blocks` returns.
 */
static int check_orphan(struct ext2_check *c, const struct ext2_node *n, int used)
{
    struct ext2_link *link = kzalloc(sizeof(*link));

    if (!link) {
        c->link_nomem = 1;
        return -ENOMEM;
    }
    link->ino = n->ino;
    link->expect = n->nlink;
    link->orphan = 1;
    link->dir = (n->mode & S_IFMT_) == S_IFDIR_;
    link->bit = !used;
    check_link_put(c, link);
    c->orphans++;
    if (link->dir) {
        return 0;
    }
    seen_set(c, n->ino);
    return check_blocks(c, n, 0);
}

/**
 * The inode scan, group by group. Every inode at or past `s_first_ino` the
 * walk did not reach is read. One with a mode and a link is a live orphan
 * (`check_orphan`), whatever its bit, since a stop leaves no live inode
 * free in the bitmap but damage may, and the allocator would write a new
 * file over it. One with no link whose bit is used is a deleted inode
 * whose bit was not yet cleared, counted with the orphans for `e2fsck` to
 * clear, its pointers not read, since they mean nothing, as `e2fsck` reads
 * such an inode. A reached inode the bitmap calls free is a fault. The free
 * inode counts, a live orphan counting as used, are recounted into the
 * descriptors in memory and into the mount's total. 0, or -EIO with the
 * fault recorded, or the -errno of a failed read or -ENOMEM.
 */
static int check_inodes(struct ext2_check *c)
{
    struct ext2_mount *em = c->em;
    struct ext2_node *n = &c->node[0];
    uint32_t g, i;
    int rc;

    em->free_inodes = 0;
    for (g = 0; g < em->groups; g++) {
        uint8_t *gd = em->gd + g * 32;
        uint32_t free = 0;

        rc = ext2_read_bytes(em, READ_QUIET, 0, (uint64_t)ext2_le32(gd + 4) * em->block_size,
                             c->bitmap, em->block_size);
        if (rc != 0) {
            return rc;
        }
        for (i = 0; i < em->ipg; i++) {
            uint32_t ino = g * em->ipg + i + 1;
            int used = (c->bitmap[i >> 3] >> (i & 7)) & 1;

            if (ino > em->inodes) {
                break;
            }
            if (seen_test(c, ino) || ino < em->first_ino) {
                if (!used) {
                    return check_fault(c, ino, "a reached inode the bitmap calls free");
                }
                continue;
            }
            rc = ext2_inode_check_load(em, ino, n);
            if (rc != 0) {
                return rc;
            }
            if (n->mode != 0 && n->nlink != 0) {
                rc = check_orphan(c, n, used);
                if (rc != 0) {
                    return rc;
                }
            } else if (used) {
                c->orphans++;
            } else {
                free++;
            }
        }
        LE16_PUT(gd + 14, free);
        em->free_inodes += free;
    }
    return 0;
}

/**
 * Walk the entries of each orphan directory not reached from another as an
 * orphan tree, with `c->root` naming it while it is walked: each entry
 * counts as a name of the inode it names (`check_child`), so that a free
 * through the tree from the root cannot take an inode an orphan directory
 * still names, and an orphan file or directory it names is no orphan. 0, or
 * -EIO with the fault recorded, or the -errno of a failed read or -ENOMEM.
 */
static int check_orphan_dirs(struct ext2_check *c)
{
    struct ext2_link *link;
    uint32_t i;
    int rc;

    for (i = 0; i < EXT2_CHECK_LINK_BUCKETS; i++) {
        for (link = c->link[i]; link; link = link->next) {
            if (!link->dir || !link->orphan || seen_test(c, link->ino)) {
                continue;
            }
            rc = ext2_inode_check_load(c->em, link->ino, &c->node[0]);
            if (rc != 0) {
                return rc;
            }
            seen_set(c, link->ino);
            c->root = link->ino;
            rc = check_dir(c, link->ino, 0, 0);
            c->root = 0;
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

/** Judge orphan directory `link`, which no entry names, as
 * `check_orphan_parents` says. 0, or -EIO with the fault or the check's
 * limit recorded. */
static int check_orphan_parent(struct ext2_check *c, const struct ext2_link *link)
{
    struct ext2_mount *em = c->em;
    struct ext2_link *up;

    if (link->empty && !link->kept) {
        if (link->parent == EXT2_ROOT_INO ||
            (link->parent >= em->first_ino && link->parent <= em->inodes)) {
            return check_count_add(c, link->ino, link->expect, 0);
        }
    } else if (link->holes) {
        return check_fault(c, link->ino, "a hole in a directory");
    } else if (link->expect != 2 + link->subdirs) {
        return check_fault(c, link->ino, "a directory link count that does not match its entries");
    } else if (link->parent == EXT2_ROOT_INO) {
        return 0;
    } else {
        up = check_link_find(c, link->parent);
        if (up && up->dir) {
            return 0;
        }
    }
    return check_fault(c, link->ino, "an orphan directory whose .. names no orphan directory");
}

/**
 * Mark `kept` each empty orphan directory that the `..` of orphan directory
 * `link`, which holds entries, names, and each that the `..` of one so
 * marked names in turn, so that `check_orphan_parent` keeps its count. Each
 * record is marked once, so a loop of `..` ends.
 */
static void check_keep_parents(struct ext2_check *c, const struct ext2_link *link)
{
    struct ext2_link *up = check_link_find(c, link->parent);

    while (up && up->dir && up->orphan && up->empty && !up->kept) {
        up->kept = 1;
        up = check_link_find(c, up->parent);
    }
}

/**
 * Judge each orphan directory no entry names, whose `..` `check_dots` and
 * whose subdirectories `check_dir_count` kept in its record unjudged.
 *
 * One that holds only `.` and `..` once the check's repairs are made
 * (`empty`) is what an rmdir or a mkdir cut off on a mount that writes back
 * leaves: the rmdir writes the removed entry at once and the inode's count
 * of 0 with the next writeback, the mkdir writes the new inode before the
 * entry that names it, and an rmdir of a tree leaves each directory's count
 * with its removed subdirectories too. Its count, whatever it is, is set to
 * 0 (`check_count_add`), after which it is a removed inode whose bit is
 * used, kept for e2fsck with the other orphans, and its `..` is not judged
 * further, but for naming an inode a volume can have: e2fsck clears such an
 * inode and reads nothing in it, so that no later rmdir of the directory
 * its `..` names, nor the reuse of that inode, which a parent removed while
 * the directory was held open allows before its count reaches the disk,
 * leaves e2fsck a `..` to repair against whatever took the inode. The
 * exception is one that the `..` of a kept orphan directory names
 * (`check_keep_parents`), which no operation of the driver leaves: set to
 * 0, it would leave that `..` naming a removed inode, which the next check
 * finds no orphan directory. It is kept as one that holds entries is.
 *
 * Any other is kept, its count, which must be two plus its subdirectories,
 * as it is, and its `..` must name the root or a directory of an orphan
 * tree (`check_orphan_dirs`), which keeps a record: inodes no path from the
 * root reaches, so that no unlink, rmdir or rename frees them while the
 * kept directory names them, and the root, which none frees. Anything else
 * is a fault: a file, which an unlink would free, or a directory the walk
 * from the root reached, which an rmdir or a rename over it that finds it
 * empty would free, since a directory's link count holds only its named
 * subdirectories, while `..` still names it. An orphan directory an entry
 * named had its `..` and its count judged by `check_dir` or `check_child`.
 * 0, or -EIO with the fault or the check's limit recorded.
 */
static int check_orphan_parents(struct ext2_check *c)
{
    struct ext2_link *link;
    uint32_t i;
    int rc;

    for (i = 0; i < EXT2_CHECK_LINK_BUCKETS; i++) {
        for (link = c->link[i]; link; link = link->next) {
            if (link->dir && link->orphan && !link->empty) {
                check_keep_parents(c, link);
            }
        }
    }
    for (i = 0; i < EXT2_CHECK_LINK_BUCKETS; i++) {
        for (link = c->link[i]; link; link = link->next) {
            if (!link->dir || !link->orphan) {
                continue;
            }
            rc = check_orphan_parent(c, link);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

/**
 * Set the bit of each live orphan the inode bitmap calls free
 * (`check_orphan`), counted in `c->iset`; the free counts already count it
 * used. 0, or the -errno of a failed read or write.
 */
static int check_inode_bits(struct ext2_check *c)
{
    struct ext2_mount *em = c->em;
    struct ext2_link *link;
    uint32_t i, g, at;
    uint8_t byte;
    int rc;

    for (i = 0; i < EXT2_CHECK_LINK_BUCKETS; i++) {
        for (link = c->link[i]; link; link = link->next) {
            if (!link->bit) {
                continue;
            }
            g = (link->ino - 1) / em->ipg;
            at = (link->ino - 1) % em->ipg;
            rc = ext2_read_bytes(em, READ_QUIET, 0,
                                 (uint64_t)ext2_le32(em->gd + g * 32 + 4) * em->block_size + at / 8,
                                 &byte, 1);
            if (rc != 0) {
                return rc;
            }
            byte = (uint8_t)(byte | (1u << (at & 7)));
            ext2_group_lock(em, g);
            rc = ext2_write_bytes(
                em, (uint64_t)ext2_le32(em->gd + g * 32 + 4) * em->block_size + at / 8, &byte, 1,
                WRITE_THROUGH | WRITE_KIND(READ_BITMAP));
            ext2_group_unlock(em, g);
            if (rc != 0) {
                return rc;
            }
            c->iset++;
        }
    }
    return 0;
}

/**
 * The orphans and the bitmap compare: the inode scan (`check_inodes`), the
 * orphan directories' entries (`check_orphan_dirs`), the `..` of those no
 * entry names and the counts of the empty ones (`check_orphan_parents`),
 * and the link counts of
 * every name found; then the block bitmaps compared twice
 * (`check_block_bitmaps`), first for a fault and then to set the bits of
 * blocks a file names and recount, and the bits of live orphans set. 0, or
 * -EIO with the fault recorded, or the -errno of a failed read or write or
 * -ENOMEM.
 */
static int check_bitmaps(struct ext2_check *c)
{
    int rc = check_inodes(c);

    if (rc == 0) {
        rc = check_orphan_dirs(c);
    }
    if (rc == 0) {
        rc = check_orphan_parents(c);
    }
    if (rc == 0) {
        rc = check_link_counts(c);
    }
    if (rc == 0) {
        rc = check_block_bitmaps(c, 0);
    }
    if (rc == 0) {
        rc = check_block_bitmaps(c, 1);
    }
    return rc != 0 ? rc : check_inode_bits(c);
}

/**
 * Set the link count of inode `ino`'s node, when one is in memory, to
 * `links`, the count the check wrote, so that a later store of the node
 * does not write the old one back. Only a remount's check finds nodes in
 * memory; it runs with admission closed, so no operation changes the
 * count meanwhile. Takes `em->share_lock`, under which the hash changes.
 */
static void check_node_links(struct ext2_mount *em, uint32_t ino, uint32_t links)
{
    struct ext2_node *n;
    uint32_t flags = spin_lock_irqsave(&em->share_lock);

    n = ext2_node_find(em, ino);
    if (n) {
        n->nlink = links;
    }
    spin_unlock_irqrestore(&em->share_lock, flags);
}

/**
 * Write the entry and link-count repairs a check without a fault recorded
 * (`check_fix_add`, `check_count_add`), in the order met, each in one write
 * through the cache, as the bitmaps' repairs are written, and log each:
 * `ext2: check: SOURCE: inode N: unused, its entry in directory D cleared`
 * for an inode number cleared, `ext2: check: SOURCE: inode N: its entry in
 * directory D of another type, type set` for a type byte set, and `ext2:
 * check: SOURCE: inode N: link count W, set to L` for a count. 0, or the
 * -errno of a failed write.
 */
static int check_fixes(struct ext2_check *c)
{
    struct ext2_mount *em = c->em;
    const struct ext2_fix *fix;
    uint8_t raw[4] = {0, 0, 0, 0};
    int rc;

    for (fix = c->fix; fix; fix = fix->next) {
        if (fix->was != 0) {
            LE16_PUT(raw, fix->links);
            rc = ext2_write_bytes(em, fix->at, raw, 2, WRITE_THROUGH | WRITE_KIND(READ_SLOT));
            raw[0] = 0;
            raw[1] = 0;
            if (rc != 0) {
                return rc;
            }
            check_node_links(em, fix->ino, fix->links);
            klog(KLOG_WARN, "ext2", "check: %s: inode %d: link count %d, set to %d",
                 em->mnt->source, (int)fix->ino, (int)fix->was, (int)fix->links);
            continue;
        }
        if (fix->ft == 0) {
            rc = ext2_write_bytes(em, fix->at, raw, 4, WRITE_THROUGH | WRITE_KIND(READ_DATA));
        } else {
            raw[0] = (uint8_t)fix->ft;
            rc = ext2_write_bytes(em, fix->at + 7, raw, 1, WRITE_THROUGH | WRITE_KIND(READ_DATA));
            raw[0] = 0;
        }
        if (rc != 0) {
            return rc;
        }
        if (fix->ft == 0) {
            klog(KLOG_WARN, "ext2",
                 "check: %s: inode %d: unused, its entry in directory %d cleared", em->mnt->source,
                 (int)fix->ino, (int)fix->dir);
        } else {
            klog(KLOG_WARN, "ext2",
                 "check: %s: inode %d: its entry in directory %d of another type, type set",
                 em->mnt->source, (int)fix->ino, (int)fix->dir);
        }
    }
    return 0;
}

/**
 * Log what a check without a fault repaired and leaves for e2fsck, setting
 * the error bit when it leaves anything: the blocks and the live inodes
 * whose bits it set, the regular files and directories whose size or
 * `i_blocks` disagrees with their pointers, the shared attribute blocks whose
 * count is above the inodes found naming them, then the lost blocks and
 * orphans; or that the volume is clean.
 */
static void check_report(struct ext2_check *c)
{
    uint32_t i;

    for (i = 0; i < c->attrs; i++) {
        if (c->attr[i].found < c->attr[i].expect) {
            c->stale++;
        }
    }
    if (c->marked != 0) {
        klog(KLOG_WARN, "ext2", "check: %d blocks named by files were free, marked used",
             (int)c->marked);
    }
    if (c->iset != 0) {
        klog(KLOG_WARN, "ext2", "check: %d live inodes were free, marked used", (int)c->iset);
    }
    if (c->sized != 0) {
        klog(KLOG_WARN, "ext2",
             "check: %d files with a size or i_blocks that does not match their blocks, kept for "
             "e2fsck",
             (int)c->sized);
        c->em->state |= EXT2_ERROR_FS;
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
    } else if (c->stale == 0 && c->sized == 0 && c->marked == 0 && c->fixes == 0) {
        klog(KLOG_INFO, "ext2", "check: clean");
    }
}

/**
 * Say that the volume stays read-only until e2fsck has run: `ext2: SOURCE:
 * read-only with errors, run e2fsck` after a fault, `ext2: SOURCE:
 * read-only unchecked, run e2fsck` after a limit of the check's.
 */
static void check_readonly(const struct ext2_mount *em, int limit)
{
    klog(KLOG_WARN, "ext2", "%s: read-only %s, run e2fsck", em->mnt->source,
         limit ? "unchecked" : "with errors");
}

/**
 * End a check that met its first fault or one of its own limits, `c->fault`:
 * the free counts put back to `free_blocks` and `free_inodes` and the
 * descriptors read again, since the recount may have stopped part way, and
 * nothing written but, for a fault of the volume, the superblock's state
 * with the error bit set; the fault logged, then that the volume stays
 * read-only (`check_readonly`). 1, or the -errno of a read or write.
 */
static int check_stopped(struct ext2_check *c, uint64_t free_blocks, uint64_t free_inodes)
{
    struct ext2_mount *em = c->em;
    uint8_t state[2];
    int rc;

    em->free_blocks = free_blocks;
    em->free_inodes = free_inodes;
    rc = ext2_read_bytes(em, READ_QUIET, 0, (uint64_t)(em->first_data + 1) * em->block_size, em->gd,
                         em->groups * 32);
    if (rc != 0) {
        return rc;
    }
    if (c->fault_ino != 0) {
        klog(KLOG_WARN, "ext2", "check: inode %d: %s", (int)c->fault_ino, c->fault);
    } else {
        klog(KLOG_WARN, "ext2", "check: %s", c->fault);
    }
    if (!c->limit) {
        ext2_super_lock(em);
        LE16_PUT(state, ext2_state_change(em, EXT2_ERROR_FS, 0));
        rc = ext2_write_bytes(em, 1024 + 58, state, 2, WRITE_DURABLE | WRITE_KIND(READ_SUPER));
        ext2_super_unlock(em);
        if (rc != 0) {
            return rc;
        }
    }
    check_readonly(em, c->limit);
    return 1;
}

/**
 * Check a volume whose valid bit is clear or whose error bit is set, at a
 * writable mount or remount, before it is written, as the section above
 * says: the structural blocks and the reserved inodes below `s_first_ino`
 * marked first, then the tree walked from the root, the orphans and their
 * entries, the link counts compared, and the bitmaps, the entries to repair
 * recorded on the way.
 *
 * Returns 0 when the check ends, and the volume may be mounted writable, as
 * Linux's ext2 mounts any volume it supports whatever its state
 * (`ext2_setup_super`): it writes the entries it repairs (`check_fixes`),
 * the bits it set of blocks files name and of live orphans and the
 * recounted free counts to the directories, the bitmaps, the descriptors
 * and the superblock, with the error bit set when blocks were
 * lost, inodes orphaned, an empty orphan directory's count set to 0 among
 * them, regular files or directories left with a size or `i_blocks` that
 * does not match their pointers or attribute blocks left counting inodes
 * that no longer name them.
 *
 * Returns 1 when the check stops, and the volume must stay read-only
 * (`check_stopped`): a fault is logged as `ext2: check: inode N: WHY` and
 * sets the error bit on the disk; a limit of the check's, its workspace
 * among them, is logged by name and leaves the state as it is. Either then
 * logs `ext2: SOURCE: read-only with errors, run e2fsck` or `ext2: SOURCE:
 * read-only unchecked, run e2fsck`. -errno when the device failed.
 */
static int check_volume(struct ext2_mount *em)
{
    struct ext2_check *c;
    uint64_t free_blocks = em->free_blocks, free_inodes = em->free_inodes;
    uint32_t g, ino;
    int rc;

    c = check_state(em);
    if (!c) {
        klog(KLOG_WARN, "ext2", "check: no memory for the %d KiB of workspace the volume needs",
             (int)((fat_walk_bytes(em->blocks) + (em->inodes + 7) / 8 + sizeof(*c)) / 1024));
        check_readonly(em, 1);
        return 1;
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
        rc = check_bitmaps(c);
    }
    fat_walk_release();
    if (rc == -ENOMEM && c->link_nomem && !c->fault) {
        (void)check_limit(c, 0, "no memory to count the hard links and orphans");
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
    rc = check_fixes(c);
    if (rc != 0) {
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
    rc = ext2_sb_store(em);
    return rc != 0 ? rc : bcache_sync(em->dev);
}

/**
 * `check_volume` with `em->opening` set, the one time a mount that is not
 * yet writable may write (`ext2_write_owned`): the repaired bitmaps and
 * recounted descriptors and superblock of a check that ends, or the error
 * bit of one that stops. The flag is given back as the caller had it, since
 * a remount writable keeps it set for the writes that follow the check.
 * Returns what `check_volume` does: 0, 1 for a volume that must stay
 * read-only, or -errno.
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
