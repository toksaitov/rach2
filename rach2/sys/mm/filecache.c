/*
 * filecache.c: the pages of ext2 files that private mappings and exec read,
 * kept between them, as filecache.h describes them.
 *
 * Exec reads a program's segments page by page, and the dynamic linker maps its
 * libraries privately, so each start reads their pages from the file again,
 * which the buffer cache, held to half the kernel's heap, does not keep for a
 * program of hundreds of megabytes. This cache keeps a copy of each page read
 * either way in a free frame, found by its file's mount, inode number and
 * generation, and checked against the size and the modification time the file
 * has when the page is asked for. A write, a truncation and an allocation
 * through the VFS drop the file's pages after they change it, and an unmount
 * every page of its files; each counts as a change, and a page read while a
 * change ran is not kept, since the read may have seen part of it. A mapping
 * gets a copy of a cached page, so nothing it does reaches the cache. Each
 * file that has pages has a record that lists them, so a change drops that
 * file's pages without a search of every other file's. The frames are the
 * machine's free memory: the cache takes one only while a quarter of the
 * frames are free, and gives them back, the least recently used first, when
 * memory runs short (`mem_reclaim`).
 *
 * The table of the pages and the table of the files each have a list for
 * every four to eight frames the machine has, a power of two set at boot, so
 * that a list stays a few entries long when the cache holds all the frames it
 * may take.
 */

#include "filecache.h"

#include "kheap.h"
#include "../lib/kstring.h"
#include "paging.h"
#include "../fs/ext2.h"
#include "../fs/fd.h"
#include "../fs/vfs.h"
#include "../lib/kirq.h"
#include "../klog.h"

/* The fewest lists that each table has. */
#define FC_MIN_LISTS 1024u

/* A file that has pages in the cache, and the list of those pages, so that a
 * change to the file drops them without a search of the other files' pages.
 * The record lives while the file has a page. */
struct fc_inode {
    struct fc_inode *next, **link; /* the slot's list, and what points here */
    const struct vfs_mount *mnt;
    uint64_t ino;
    struct fc_page *pages;
};

/* A cached page: which page of which file, the size and the modification
 * time the file had when the page was read, and the frame of the copy. */
struct fc_page {
    struct fc_page *next, **link;            /* the bucket's list */
    struct fc_page *sibling, **sibling_link; /* the file's list */
    struct fc_page *older, *younger;         /* the use order */
    struct fc_inode *file;
    uint32_t generation;
    uint32_t pgoff;
    uint64_t size;
    int64_t mtime;
    uintptr_t phys;
};

static struct fc_page **buckets;
static struct fc_inode **files;
static uint32_t table_mask; /* the lists of each table, less one */
static struct fc_page *oldest, *youngest;
static uint32_t pages;
static uint32_t changes; /* `filecache_epoch` */

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

void init_filecache(void)
{
    uint32_t lists = FC_MIN_LISTS;

    while (lists < 0x40000000u && (size_t)lists * 2u <= frame_total_count() / 4u) {
        lists *= 2u;
    }
    buckets = kzalloc(lists * sizeof(*buckets));
    files = kzalloc(lists * sizeof(*files));
    if (!buckets || !files) {
        kfree(buckets);
        kfree(files);
        buckets = NULL;
        files = NULL;
        klog(KLOG_WARN, "filecache", "tables: no room for %u lists; no page is cached",
             (unsigned)lists);
        return;
    }
    table_mask = lists - 1u;
}

/** A hash of the file `ino` of `mnt`, its bits spread over the whole word. */
static uint32_t fc_hash(const struct vfs_mount *mnt, uint64_t ino)
{
    uint32_t h = (uint32_t)(uintptr_t)mnt >> 4 ^ (uint32_t)ino ^ (uint32_t)(ino >> 32) * 40503u;

    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return h;
}

/** The bucket of page `pgoff` of the file `ino` of `mnt`; a file's pages
 * take consecutive buckets. */
static uint32_t fc_bucket(const struct vfs_mount *mnt, uint64_t ino, uint32_t pgoff)
{
    return (fc_hash(mnt, ino) + pgoff) & table_mask;
}

/** The slot of `files` for the file `ino` of `mnt`. */
static uint32_t fc_slot(const struct vfs_mount *mnt, uint64_t ino)
{
    return fc_hash(mnt, ino) & table_mask;
}

/** The record of the file `ino` of `mnt`, or NULL when the cache has none
 * of its pages. Interrupts must be off. */
static struct fc_inode *fc_find(const struct vfs_mount *mnt, uint64_t ino)
{
    struct fc_inode *file;

    for (file = files[fc_slot(mnt, ino)]; file; file = file->next) {
        if (file->mnt == mnt && file->ino == ino) {
            return file;
        }
    }
    return NULL;
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

/** Put `p`, which is not in the use order, at its young end. */
static void fc_add_use(struct fc_page *p)
{
    p->older = youngest;
    if (youngest) {
        youngest->younger = p;
    } else {
        oldest = p;
    }
    youngest = p;
}

/** Make `p`, which is in the use order, the most recently used. */
static void fc_use(struct fc_page *p)
{
    fc_unlink_use(p);
    fc_add_use(p);
}

/** Free `p`, and its file's record when it was the file's last page. */
static void fc_drop(struct fc_page *p)
{
    struct fc_inode *file = p->file;

    *p->link = p->next;
    if (p->next) {
        p->next->link = p->link;
    }
    *p->sibling_link = p->sibling;
    if (p->sibling) {
        p->sibling->sibling_link = p->sibling_link;
    }
    fc_unlink_use(p);
    free_frame(p->phys);
    kfree(p);
    pages--;
    if (!file->pages) {
        *file->link = file->next;
        if (file->next) {
            file->next->link = file->link;
        }
        kfree(file);
    }
}

/** Drop every page of `file`, and with the last of them the record. */
static void fc_drop_file(struct fc_inode *file)
{
    struct fc_page *p, *next;

    for (p = file->pages; p; p = next) {
        next = p->sibling;
        fc_drop(p);
    }
}

/** Copy a page to the frame `to`, or to `to_buf` when it is 0, from the
 * frame `from`, or from `from_buf` when it is 0; 1 when it was copied. */
static int fc_copy(uintptr_t to, void *to_buf, uintptr_t from, const void *from_buf)
{
    uint8_t *page;

    if (to && from) {
        return paging_copy_frame(to, from) == 0;
    }
    page = kmap_temp(to ? to : from);
    if (!page) {
        return 0;
    }
    if (to) {
        k_memcpy(page, from_buf, PAGE_SIZE);
    } else {
        k_memcpy(to_buf, page, PAGE_SIZE);
    }
    kunmap_temp(page);
    return 1;
}

/** Copy page `pgoff` of `f` into the frame `phys`, or into `buf` when it is
 * 0, when the cache has it as the file is now; 1 then. */
static int fc_read(const struct fc_file *f, uint32_t pgoff, uintptr_t phys, void *buf)
{
    struct fc_page *p;
    uint32_t flags = kirq_save();
    int hit = 0;

    for (p = buckets[fc_bucket(f->mnt, f->ino, pgoff)]; p; p = p->next) {
        if (p->file->mnt == f->mnt && p->file->ino == f->ino && p->pgoff == pgoff &&
            p->generation == f->generation && p->size == f->size && p->mtime == f->mtime) {
            hit = fc_copy(phys, buf, p->phys, NULL);
            if (hit) {
                fc_use(p);
            }
            break;
        }
    }
    kirq_restore(flags);
    return hit;
}

/** Keep a copy of page `pgoff` of `f`, from the frame `phys` or, when it is
 * 0, from `buf`, in place of any it had, unless fewer than a quarter of the
 * frames are free or a change has been recorded since `epoch`. */
static void fc_store(const struct fc_file *f, uint32_t pgoff, uintptr_t phys, const void *buf,
                     uint32_t epoch)
{
    struct fc_page *p, *old, **bucket;
    struct fc_inode *file, *spare;
    uint32_t flags;

    if (!buckets || frame_free_count() < frame_total_count() / 4u) {
        return;
    }
    p = kzalloc(sizeof(*p));
    spare = kzalloc(sizeof(*spare));
    if (!p || !spare) {
        kfree(p);
        kfree(spare);
        return;
    }
    p->phys = alloc_user_frame();
    if (p->phys == 0 || !fc_copy(p->phys, NULL, phys, buf)) {
        if (p->phys) {
            free_frame(p->phys);
        }
        kfree(p);
        kfree(spare);
        return;
    }
    p->generation = f->generation;
    p->pgoff = pgoff;
    p->size = f->size;
    p->mtime = f->mtime;
    flags = kirq_save();
    if (epoch != changes) {
        kirq_restore(flags);
        free_frame(p->phys);
        kfree(p);
        kfree(spare);
        return;
    }
    bucket = &buckets[fc_bucket(f->mnt, f->ino, pgoff)];
    for (old = *bucket; old; old = old->next) {
        if (old->file->mnt == f->mnt && old->file->ino == f->ino && old->pgoff == pgoff) {
            fc_drop(old);
            break;
        }
    }
    file = fc_find(f->mnt, f->ino);
    if (!file) {
        file = spare;
        spare = NULL;
        file->mnt = f->mnt;
        file->ino = f->ino;
        file->link = &files[fc_slot(f->mnt, f->ino)];
        file->next = *file->link;
        if (file->next) {
            file->next->link = &file->next;
        }
        *file->link = file;
    }
    p->file = file;
    p->link = bucket;
    p->next = *bucket;
    if (p->next) {
        p->next->link = &p->next;
    }
    *bucket = p;
    p->sibling_link = &file->pages;
    p->sibling = file->pages;
    if (p->sibling) {
        p->sibling->sibling_link = &p->sibling;
    }
    file->pages = p;
    fc_add_use(p);
    pages++;
    kirq_restore(flags);
    kfree(spare);
}

int filecache_read(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys)
{
    struct fc_file f;

    return pages && e->ofd && fc_file_of(e->ofd->vmnt, e->ofd->vnode, &f) &&
           fc_read(&f, pgoff, phys, NULL);
}

int filecache_read_node(struct vfs_mount *mnt, void *node, uint32_t pgoff, void *buf)
{
    struct fc_file f;

    return pages && fc_file_of(mnt, node, &f) && fc_read(&f, pgoff, 0, buf);
}

uint32_t filecache_epoch(void)
{
    return changes;
}

void filecache_store(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    struct fc_file f;

    if (e->ofd && fc_file_of(e->ofd->vmnt, e->ofd->vnode, &f)) {
        fc_store(&f, pgoff, phys, NULL, epoch);
    }
}

void filecache_store_node(struct vfs_mount *mnt, void *node, uint32_t pgoff, const void *buf,
                          uint32_t epoch)
{
    struct fc_file f;

    if (fc_file_of(mnt, node, &f)) {
        fc_store(&f, pgoff, 0, buf, epoch);
    }
}

/** Drop every page that `mnt` and, unless it is 0, `ino` name. */
static void fc_forget(const struct vfs_mount *mnt, uint64_t ino)
{
    struct fc_inode *file, *next;
    uint32_t i, flags = kirq_save();

    if (ino != 0) {
        file = fc_find(mnt, ino);
        if (file) {
            fc_drop_file(file);
        }
    } else {
        for (i = 0; pages && i <= table_mask; i++) {
            for (file = files[i]; file; file = next) {
                next = file->next;
                if (file->mnt == mnt) {
                    fc_drop_file(file);
                }
            }
        }
    }
    kirq_restore(flags);
}

void filecache_forget(struct vfs_mount *mnt, void *node)
{
    struct fc_file f;

    changes++;
    if (pages && fc_file_of(mnt, node, &f)) {
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
        fc_drop(oldest);
        given++;
    }
    kirq_restore(flags);
    return given;
}
