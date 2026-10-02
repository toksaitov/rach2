/*
 * filecache.c: the pages of ext2 files that private mappings read, kept
 * between mappings, as filecache.h describes them.
 *
 * A program's text is mapped privately, and each start reads its pages from
 * the file again into frames of its own, which the buffer cache, held to
 * half the kernel's heap, does not keep for a program of hundreds of
 * megabytes. This cache keeps a copy of each page read that way in a free
 * frame, found by its file's mount, inode number and generation, and checked
 * against the size and the modification time the file has when the page is
 * asked for. A write, a truncation and an allocation through the VFS drop
 * the file's pages after they change it, and an unmount every page of its
 * files; each counts as a change, and a page read while a change ran is
 * not kept, since the read may have seen part of it. A mapping gets a copy
 * of a cached page, so nothing it does reaches the cache. The
 * frames are the machine's free memory: the cache takes one only while a
 * quarter of the frames are free, and gives them back, the least recently
 * used first, when memory runs short (`mem_reclaim`).
 */

#include "filecache.h"

#include "kheap.h"
#include "paging.h"
#include "../fs/ext2.h"
#include "../fs/fd.h"
#include "../fs/vfs.h"
#include "../lib/kirq.h"

#define FC_BUCKETS 1024u
#define FC_FILES   256u

struct fc_page {
    struct fc_page *next;            /* the bucket's list */
    struct fc_page *older, *younger; /* the use order */
    const struct vfs_mount *mnt;
    uint64_t ino;
    uint32_t generation;
    uint32_t pgoff;
    uint64_t size;
    int64_t mtime;
    uintptr_t phys;
};

static struct fc_page *buckets[FC_BUCKETS];
static struct fc_page *oldest, *youngest;
static uint32_t pages;
static uint32_t file_pages[FC_FILES]; /* the pages of the files of each `fc_slot` */
static uint32_t changes;              /* `filecache_epoch` */

/* What finds a file's pages and what they must still match. */
struct fc_file {
    const struct vfs_mount *mnt;
    uint64_t ino;
    uint32_t generation;
    uint64_t size;
    int64_t mtime;
};

/** The identity of the file `node` of `mnt`, 1 for an ext2 regular file,
 * the one type whose inode numbers and generations name one file. */
static int fc_file_of(struct vfs_mount *mnt, void *node, struct fc_file *f)
{
    struct vfs_stat st;

    if (!mnt || !node || vfs_mount_base(mnt)->type != &vfs_ext2_type ||
        vfs_getattr(mnt, node, &st) != 0 || (st.mode & 0170000u) != 0100000u) {
        return 0;
    }
    f->mnt = vfs_mount_base(mnt);
    f->ino = st.ino;
    f->generation = st.generation;
    f->size = st.size;
    f->mtime = st.mtime_sec;
    return 1;
}

static uint32_t fc_bucket(const struct vfs_mount *mnt, uint64_t ino, uint32_t pgoff)
{
    return ((uint32_t)(uintptr_t)mnt >> 4 ^ (uint32_t)ino * 2654435761u ^ pgoff * 40503u) %
           FC_BUCKETS;
}

/** The slot of `file_pages` for the file `ino` of `mnt`. */
static uint32_t fc_slot(const struct vfs_mount *mnt, uint64_t ino)
{
    return ((uint32_t)ino * 2654435761u >> 24 ^ (uint32_t)(uintptr_t)mnt >> 6) % FC_FILES;
}

/** Take `p` out of the use order. */
static void fc_unlink_use(struct fc_page *p)
{
    if (p->older) {
        p->older->younger = p->younger;
    } else {
        oldest = p->younger;
    }
    if (p->younger) {
        p->younger->older = p->older;
    } else {
        youngest = p->older;
    }
    p->older = p->younger = NULL;
}

/** Make `p` the most recently used. */
static void fc_use(struct fc_page *p)
{
    fc_unlink_use(p);
    p->older = youngest;
    if (youngest) {
        youngest->younger = p;
    } else {
        oldest = p;
    }
    youngest = p;
}

/** Free `p`, which `link` points to in its bucket. */
static void fc_drop(struct fc_page **link)
{
    struct fc_page *p = *link;

    *link = p->next;
    fc_unlink_use(p);
    file_pages[fc_slot(p->mnt, p->ino)]--;
    free_frame(p->phys);
    kfree(p);
    pages--;
}

int filecache_read(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys)
{
    struct fc_file f;
    struct fc_page *p;
    uint32_t flags;
    int hit = 0;

    if (!pages || !e->ofd || !fc_file_of(e->ofd->vmnt, e->ofd->vnode, &f)) {
        return 0;
    }
    flags = kirq_save();
    for (p = buckets[fc_bucket(f.mnt, f.ino, pgoff)]; p; p = p->next) {
        if (p->mnt == f.mnt && p->ino == f.ino && p->pgoff == pgoff &&
            p->generation == f.generation && p->size == f.size && p->mtime == f.mtime) {
            hit = paging_copy_frame(phys, p->phys) == 0;
            if (hit) {
                fc_use(p);
            }
            break;
        }
    }
    kirq_restore(flags);
    return hit;
}

uint32_t filecache_epoch(void)
{
    return changes;
}

void filecache_store(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    struct fc_file f;
    struct fc_page *p, **link;
    uint32_t flags;

    if (frame_free_count() < frame_total_count() / 4u || !e->ofd ||
        !fc_file_of(e->ofd->vmnt, e->ofd->vnode, &f)) {
        return;
    }
    p = kzalloc(sizeof(*p));
    if (!p) {
        return;
    }
    p->phys = alloc_user_frame();
    if (p->phys == 0 || paging_copy_frame(p->phys, phys) != 0) {
        if (p->phys) {
            free_frame(p->phys);
        }
        kfree(p);
        return;
    }
    p->mnt = f.mnt;
    p->ino = f.ino;
    p->generation = f.generation;
    p->pgoff = pgoff;
    p->size = f.size;
    p->mtime = f.mtime;
    flags = kirq_save();
    if (epoch != changes) {
        kirq_restore(flags);
        free_frame(p->phys);
        kfree(p);
        return;
    }
    link = &buckets[fc_bucket(f.mnt, f.ino, pgoff)];
    while (*link && !((*link)->mnt == f.mnt && (*link)->ino == f.ino && (*link)->pgoff == pgoff)) {
        link = &(*link)->next;
    }
    if (*link) {
        fc_drop(link);
    }
    p->next = buckets[fc_bucket(f.mnt, f.ino, pgoff)];
    buckets[fc_bucket(f.mnt, f.ino, pgoff)] = p;
    fc_use(p);
    file_pages[fc_slot(f.mnt, f.ino)]++;
    pages++;
    kirq_restore(flags);
}

/** Drop every page that `mnt` and, unless it is 0, `ino` name. */
static void fc_forget(const struct vfs_mount *mnt, uint64_t ino)
{
    uint32_t i, flags = kirq_save();

    for (i = 0; pages && i < FC_BUCKETS; i++) {
        struct fc_page **link = &buckets[i];

        while (*link) {
            if ((*link)->mnt == mnt && (ino == 0 || (*link)->ino == ino)) {
                fc_drop(link);
            } else {
                link = &(*link)->next;
            }
        }
    }
    kirq_restore(flags);
}

void filecache_forget(struct vfs_mount *mnt, void *node)
{
    struct fc_file f;

    changes++;
    if (pages && fc_file_of(mnt, node, &f) && file_pages[fc_slot(f.mnt, f.ino)]) {
        fc_forget(f.mnt, f.ino);
    }
}

void filecache_forget_mount(const struct vfs_mount *mnt)
{
    changes++;
    if (pages) {
        fc_forget(vfs_mount_base(mnt), 0);
    }
}

uint32_t filecache_pages(void)
{
    return pages;
}

uint32_t filecache_shrink(uint32_t want)
{
    uint32_t given = 0, flags = kirq_save();

    while (oldest && given < want) {
        struct fc_page *p = oldest;
        struct fc_page **link = &buckets[fc_bucket(p->mnt, p->ino, p->pgoff)];

        while (*link != p) {
            link = &(*link)->next;
        }
        fc_drop(link);
        given++;
    }
    kirq_restore(flags);
    return given;
}
