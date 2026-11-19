/*
 * filecache.c: the pages of ext2 files that read(2), private mappings and
 * exec read, kept between them, as filecache.h describes them.
 *
 * The buffer cache, held to half the kernel's heap, keeps at most a gigabyte of
 * a machine of 16 GiB, so a file read again, a program started again or a
 * library mapped again would come from the disk again. This cache keeps a copy
 * of each page read any of those ways in a free frame, found by its file's
 * mount, inode number and generation, and checked against the size and the
 * modification time the file has when the page is asked for. A write and a
 * truncation through the VFS drop the pages they changed after they change the
 * file, and keep its others, an allocation and a change of size every page of
 * the file, and an unmount every page of its files; each counts as a change of
 * the file, and a page read while a change of its file ran is not kept, since
 * the read may have seen part of it. A private mapping gets a copy of a cached
 * page. The pages in memory of a disk file that no shared mapping through a
 * writable descriptor maps share the cache's frame, as Linux's page cache gives
 * read(2) and every mapping one folio: the file's pages change only as a write
 * or a truncation changes the file, which drops the pages it changed here
 * after, and such a mapping, whose stores reach the pages before the file,
 * drops them when it is made. Each file that has pages has a record that lists
 * them and holds the size and the modification time they were read at, which a
 * write or a truncation moves to the file's new ones once it has dropped the
 * pages it changed, so a change drops that file's pages without a search of
 * every other file's. The frames are the machine's free memory: the cache takes
 * a new one only while a quarter of the frames are free and its records fit in
 * an eighth of the heap, and past either uses the frame of its least recently
 * used page; it gives frames back, the least recently used first, when memory
 * runs short (`mem_reclaim`).
 *
 * The table of the pages and the table of the files each have a list for
 * every four to eight frames the machine has, a power of two set at boot, so
 * that a list stays a few entries long when the cache holds all the frames it
 * may take.
 *
 * `fc_lock`, a spinlock taken with interrupts off, covers both tables, the
 * mount lists, the use order, the count and the counters of changes. Whole
 * files are detached from lookup before their pages are freed in batches.
 * The copies of pages and the reads of files run outside it, the frame of a
 * page being read held by a reference. It comes after the page-table locks
 * and before the temporary kernel slots, whose `kmap_temp` a hit copies
 * under it, as Linux takes the `i_pages` lock under the page-table lock
 * (mm/filemap.c:89-92), and no caller needs the big lock.
 */

#include "filecache.h"

#include "kheap.h"
#include "../lib/kstring.h"
#include "paging.h"
#include "usercopy.h"
#include "../fs/ext2.h"
#include "../fs/fd.h"
#include "../fs/vfs.h"
#include "../lib/kerrno.h"
#include "../lib/spinlock.h"
#include "../block/block.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#include "../proc/wait_queue.h"
#include "../arch/arch.h"
#include "../klog.h"

/* The fewest lists that each table has. */
#define FC_MIN_LISTS 1024u

/* The pages a run read in one call holds on the stack: Linux's default
 * readahead window. */
#define FC_WINDOW FILE_RA_PAGES

/* The counters of changes, one for each set of files whose hashes share
 * their low bits, so that a write to one file costs the reads of only the
 * few files beside it the pages they were reading. A power of two. */
#define FC_EPOCHS 256u

/* The entries of a file's list an invalidation examines in one hold of
 * `fc_lock`, the pages it keeps and the cursors it passes included. A host
 * test sets a smaller batch of its own. */
#ifndef FC_DROP_BATCH
#define FC_DROP_BATCH 128u
#endif

/* A file that has pages in the cache, the list of those pages, so that a
 * change to the file drops them without a search of the other files' pages,
 * and what the file was when they were read. The record lives while the
 * file has a page or a range walk holds it, and a detached record until
 * both its drain and its last range walk are done. */
struct fc_inode {
    struct fc_inode *next, **link;             /* the slot's list; link is NULL once detached */
    struct fc_inode *mount_next, **mount_link; /* the mount's cached inodes */
    struct vfs_mount *mnt;
    uint64_t ino;
    uint64_t size;
    int64_t mtime;
    uint32_t generation;
    uint32_t walks;   /* range walks that hold the record: one walking, the rest waiting */
    uint32_t walking; /* 1 while a range walk's cursor is on `pages` */
    uint32_t drained; /* 1 once a detached record's drain has ended */
    struct fc_page *pages;
};

/* A cached page: which page of which file and the frame of the copy. The
 * record is kept to 64 bytes on x86-64 and 32 on i686, a block of the
 * heap's class of that size, since the cache may hold millions. A range
 * walk's cursor is a record on the file's list alone, with `file` NULL. */
struct fc_page {
    struct fc_page *next;                    /* the bucket's list */
    struct fc_page *sibling, **sibling_link; /* the file's list */
    struct fc_page *older, *younger;         /* the use order */
    struct fc_inode *file;
    uint32_t pgoff;
    /* The frame's number, its address over PAGE_SIZE, FC_MARK and FC_LOCK. */
    uint32_t pfn;
};

/* The bits of `fc_page.pfn` that mark the page whose read starts the next
 * readahead window, Linux's PG_readahead, and a page whose read has not
 * ended, which no reader copies or maps, Linux's PG_locked on a folio not
 * yet up to date; the bits below them are the frame's number, so a frame
 * at or past 4 TiB is not kept. */
#define FC_MARK     0x80000000u
#define FC_LOCK     0x40000000u
#define FC_PFN_MASK 0x3FFFFFFFu

/** The address of the frame of `p`. */
static uintptr_t fc_phys(const struct fc_page *p)
{
    return (uintptr_t)(p->pfn & FC_PFN_MASK) * PAGE_SIZE;
}

static struct spinlock fc_lock = SPINLOCK_INIT_NAMED("filecache");
static struct fc_page **buckets;
static struct fc_inode **files;
static uint32_t table_mask; /* the lists of each table, less one */
static struct fc_page *oldest, *youngest;
static uint32_t pages;
static uint32_t most;               /* the most pages whose records the heap gives */
static uint32_t changes[FC_EPOCHS]; /* `filecache_epoch` */

/* What finds a file's pages and what they must still match. */
struct fc_file {
    struct vfs_mount *mnt;
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
    struct vfs_mount *base;

    if (!mnt || !node) {
        return 0;
    }
    base = mnt->bind_of ? mnt->bind_of : mnt;
    if (base->type != &vfs_ext2_type || vfs_getattr(mnt, node, &st) != 0 ||
        (st.mode & 0170000u) != 0100000u) {
        return 0;
    }
    f->mnt = base;
    f->ino = st.ino;
    f->generation = st.generation;
    f->size = st.size;
    f->mtime = st.mtime_sec;
    return 1;
}

void init_filecache(void)
{
    uint32_t lists = FC_MIN_LISTS, record = 16u;

    while (lists < 0x40000000u && (size_t)lists * 2u <= frame_total_count() / 4u) {
        lists *= 2u;
    }
    while (record < sizeof(struct fc_page)) {
        record *= 2u;
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
    most = kheap_region_pages() * (PAGE_SIZE / 8u / record);
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
static struct fc_page **fc_bucket(const struct vfs_mount *mnt, uint64_t ino, uint32_t pgoff)
{
    return &buckets[(fc_hash(mnt, ino) + pgoff) & table_mask];
}

/** The slot of `files` for the file `ino` of `mnt`. */
static uint32_t fc_slot(const struct vfs_mount *mnt, uint64_t ino)
{
    return fc_hash(mnt, ino) & table_mask;
}

/** The counter of changes of the file `ino` of `mnt`. */
static uint32_t *fc_changes(const struct vfs_mount *mnt, uint64_t ino)
{
    return &changes[fc_hash(mnt, ino) & (FC_EPOCHS - 1u)];
}

/** Link a file to the inode table and its mount. Under `fc_lock`. */
static void fc_link_file(struct fc_inode *file)
{
    file->link = &files[fc_slot(file->mnt, file->ino)];
    file->next = *file->link;
    if (file->next) {
        file->next->link = &file->next;
    }
    *file->link = file;
    file->mount_link = &file->mnt->filecache_files;
    file->mount_next = *file->mount_link;
    if (file->mount_next) {
        file->mount_next->mount_link = &file->mount_next;
    }
    *file->mount_link = file;
}

/** Remove `file` from the inode table and its mount's list. Its pages cease
 * to answer lookups and the caller owns the record. Under `fc_lock`. */
static void fc_detach_file(struct fc_inode *file)
{
    *file->link = file->next;
    if (file->next) {
        file->next->link = file->link;
    }
    file->link = NULL;
    *file->mount_link = file->mount_next;
    if (file->mount_next) {
        file->mount_next->mount_link = file->mount_link;
    }
}

/** The record of the file `ino` of `mnt`, or NULL when the cache has none
 * of its pages. Under `fc_lock`. */
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

/** Whether the pages of `file` were read from `f` as it is now. */
static int fc_current(const struct fc_inode *file, const struct fc_file *f)
{
    return file->generation == f->generation && file->size == f->size && file->mtime == f->mtime;
}

/** Page `pgoff` of `f` as the file is now, or NULL. Under `fc_lock`. */
static struct fc_page *fc_lookup(const struct fc_file *f, uint32_t pgoff)
{
    struct fc_page *p;

    for (p = *fc_bucket(f->mnt, f->ino, pgoff); p; p = p->next) {
        if (p->pgoff == pgoff && p->file->link && p->file->mnt == f->mnt &&
            p->file->ino == f->ino) {
            return fc_current(p->file, f) ? p : NULL;
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

/** Take `p` off every list and free its file's record if still in the inode
 * table, this is its last page and no range walk holds it. The page and
 * frame are the caller's. */
static void fc_unlink(struct fc_page *p)
{
    struct fc_inode *file = p->file;
    struct fc_page **at = fc_bucket(file->mnt, file->ino, p->pgoff);

    while (*at != p) {
        at = &(*at)->next;
    }
    *at = p->next;
    *p->sibling_link = p->sibling;
    if (p->sibling) {
        p->sibling->sibling_link = p->sibling_link;
    }
    fc_unlink_use(p);
    pages--;
    if (!file->pages && file->link && !file->walks) {
        fc_detach_file(file);
        kfree(file);
    }
}

/** Free `p` after unlinking it. Return 1 when its frame went back to the
 * pool, 0 when a reader still holds it. */
static int fc_drop(struct fc_page *p)
{
    uint32_t left;

    fc_unlink(p);
    left = frame_unref(fc_phys(p));
    kfree(p);
    return left == 0;
}

/** Count a unit of the walk and give way between batches, returning with
 * `fc_lock` taken again and `*flags` set for the next restore. */
static void fc_drop_step(uint32_t *steps, uint32_t *flags)
{
    if (++*steps == FC_DROP_BATCH) {
        spin_unlock_irqrestore(&fc_lock, *flags);
        sched_give_way();
        *flags = spin_lock_irqsave(&fc_lock);
        *steps = 0;
    }
}

/** Put the cursor `c` on a file's list at `*at`. Under `fc_lock`. */
static void fc_cursor_put(struct fc_page *c, struct fc_page **at)
{
    c->sibling_link = at;
    c->sibling = *at;
    if (c->sibling) {
        c->sibling->sibling_link = &c->sibling;
    }
    *at = c;
}

/** Take the cursor `c` off its file's list. Under `fc_lock`. */
static void fc_cursor_take(struct fc_page *c)
{
    *c->sibling_link = c->sibling;
    if (c->sibling) {
        c->sibling->sibling_link = c->sibling_link;
    }
}

/**
 * Drop the pages of `file` from `first` to `last`, walking its list once
 * from the head with the caller's cursor `c` and counting each entry
 * passed, a page kept or another walk's cursor as much as a page dropped,
 * against `*steps`, which `fc_drop_step` turns into a give of `fc_lock`
 * after each batch. The
 * cursor sits behind the last entry examined while the lock is out, and a
 * drop of any entry fixes the link of what follows it, so the walk goes on
 * from its place whatever reclaim or another walk took meanwhile; a page
 * added meanwhile goes to the head, behind the walk. Called and returned
 * with `fc_lock` held.
 */
static void fc_walk(struct fc_inode *file, struct fc_page *c, uint64_t first, uint64_t last,
                    uint32_t *steps, uint32_t *flags)
{
    struct fc_page *p;

    k_memset(c, 0, sizeof(*c));
    fc_cursor_put(c, &file->pages);
    while ((p = c->sibling) != NULL) {
        fc_cursor_take(c);
        fc_cursor_put(c, &p->sibling);
        if (p->file && p->pgoff >= first && p->pgoff <= last) {
            fc_drop(p);
        }
        fc_drop_step(steps, flags);
    }
    fc_cursor_take(c);
}

/** Drop every page of the detached `file` (`fc_walk`) and free its record,
 * or leave the record to its last range walk when one holds it. No page
 * joins a detached record, so one pass leaves its list empty but for the
 * cursor of a walk still on it. */
static void fc_drain_file(struct fc_inode *file, uint32_t *steps, uint32_t *flags)
{
    struct fc_page cursor;

    fc_walk(file, &cursor, 0, ~(uint64_t)0, steps, flags);
    if (file->walks) {
        file->drained = 1;
    } else {
        kfree(file);
    }
}

/** Invalidate the whole file, then drain its pages in bounded batches.
 * Called and returned with `fc_lock` held. */
static void fc_drop_file(struct fc_inode *file, uint32_t *steps, uint32_t *flags)
{
    fc_detach_file(file);
    fc_drain_file(file, steps, flags);
}

/* The queues a range walk waits on for another walk of its file to end,
 * hashed by file. A power of two. */
#define FC_WALK_WAITS 16u

static wait_queue_t walk_wq[FC_WALK_WAITS];

/** The queue the range walks of `file` wait on. */
static wait_queue_t *fc_walk_wq(const struct fc_inode *file)
{
    return &walk_wq[fc_hash(file->mnt, file->ino) & (FC_WALK_WAITS - 1u)];
}

/** Whether no range walk of the file `arg`, a record the waiter holds,
 * runs. */
static int fc_walk_ended(void *arg)
{
    const struct fc_inode *file = arg;
    uint32_t flags = spin_lock_irqsave(&fc_lock);
    int ended = !file->walking;

    spin_unlock_irqrestore(&fc_lock, flags);
    return ended;
}

/** Let go of a range walk's hold on `file`, and free the record when it
 * was the last hold and the list is empty: a record still in the table is
 * taken out of it, and a detached one is freed only once its drain has
 * ended, since until then the drain's cursor is on the list. Under
 * `fc_lock`. */
static void fc_walk_put(struct fc_inode *file)
{
    if (--file->walks == 0 && !file->pages) {
        if (file->link) {
            fc_detach_file(file);
            kfree(file);
        } else if (file->drained) {
            kfree(file);
        }
    }
}

/**
 * Drop the pages of `file` from `first` to `last` (`fc_walk`), examining at
 * most FC_DROP_BATCH entries of its list in one hold of `fc_lock`, as
 * Linux's truncate and invalidate loops give way between batches. One
 * range walk of a file runs at a time, and the others wait with the lock
 * let go, as Linux's truncations of a file take its `i_rwsem`: walks that
 * passed each other's cursors would each count the others against their
 * batch, and with more than a batch of them on a list that reclaim had
 * emptied, the walks at the head could pass the others to the tail in turn
 * and never reach it. A walk, running or waiting, holds the record, which
 * lives while one does (`fc_walk_put`), and a drain leaves it to the last.
 * Called and returned with `fc_lock` held; may sleep, so the caller holds
 * no other spinlock.
 */
static void fc_drop_range(struct fc_inode *file, uint64_t first, uint64_t last, uint32_t *flags)
{
    struct fc_page cursor;
    wait_queue_t *wq = fc_walk_wq(file);
    uint32_t steps = 0;
    int waiting;

    file->walks++;
    while (file->walking) {
        spin_unlock_irqrestore(&fc_lock, *flags);
        (void)wq_wait_event_nointr(wq, fc_walk_ended, file);
        *flags = spin_lock_irqsave(&fc_lock);
    }
    file->walking = 1;
    fc_walk(file, &cursor, first, last, &steps, flags);
    file->walking = 0;
    waiting = file->walks > 1;
    fc_walk_put(file);
    if (waiting) {
        spin_unlock_irqrestore(&fc_lock, *flags);
        wq_wake_all(wq);
        *flags = spin_lock_irqsave(&fc_lock);
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
 * 0, when the cache has it as the file is now and its read has ended; 1
 * then. */
static int fc_read(const struct fc_file *f, uint32_t pgoff, uintptr_t phys, void *buf)
{
    struct fc_page *p;
    uint32_t flags = spin_lock_irqsave(&fc_lock);
    int hit = 0;

    p = fc_lookup(f, pgoff);
    if (p && !(p->pfn & FC_LOCK)) {
        hit = fc_copy(phys, buf, fc_phys(p), NULL);
        if (hit) {
            fc_use(p);
        }
    }
    spin_unlock_irqrestore(&fc_lock, flags);
    return hit;
}

/** A page record off every list, with no frame yet, for a new page: a new
 * record while a quarter of the frames are free and the records fit in an
 * eighth of the heap; past either, the least recently used page's record,
 * whose frame goes back, while a sixteenth of the frames are free, above
 * the reserve that `mem_may_take` keeps; NULL otherwise. Under `fc_lock`. */
static struct fc_page *fc_record(void)
{
    struct fc_page *p;
    size_t free_frames = frame_free_count(), total = frame_total_count();

    if (pages < most && free_frames >= total / 4u) {
        return kzalloc(sizeof(*p));
    }
    if (oldest && free_frames >= total / 16u) {
        p = oldest;
        fc_unlink(p);
        /* A reader of the page holds a reference of its own. */
        free_frame(fc_phys(p));
        k_memset(p, 0, sizeof(*p));
        return p;
    }
    return NULL;
}

/** A page record off every list, as `fc_record` gives one, with a new
 * frame of its own, so that the frame given back for a used record is
 * there to be had; NULL otherwise. Under `fc_lock`. */
static struct fc_page *fc_new(void)
{
    struct fc_page *p = fc_record();
    uintptr_t phys;

    if (!p) {
        return NULL;
    }
    phys = alloc_user_frame();
    if (phys == 0 || phys / PAGE_SIZE > FC_PFN_MASK) {
        if (phys != 0) {
            free_frame(phys);
        }
        kfree(p);
        return NULL;
    }
    p->pfn = (uint32_t)(phys / PAGE_SIZE);
    return p;
}

/** Free `p`, which `fc_new` gave and is on no list, and its frame. */
static void fc_free(struct fc_page *p)
{
    free_frame(fc_phys(p));
    kfree(p);
}

/**
 * Put `p`, which `fc_new` gave and whose frame holds page `pgoff` of `f` as
 * just read, into the cache, unless a change of the file has been recorded
 * since `epoch` or the cache holds that page of the file already; a page
 * read from the file as it was before is dropped with the rest of the
 * file's pages. 1, or 0 when `p` stays the caller's.
 *
 * A page already held is kept, since a change of the file drops its pages
 * and so the one there is as current as `p`: two readers that miss the same
 * page both read it, and the second's copy goes. Linux's
 * `__filemap_add_folio` likewise fails with -EEXIST on a folio present in
 * the mapping (mm/filemap.c:872-876), and `filemap_create_folio` then puts
 * its own folio and has its caller look the page up again
 * (mm/filemap.c:2495-2511).
 *
 * A new file record is allocated with `fc_lock` let go, which keeps the
 * heap's work out of the hold. Whether one is needed is read under the
 * first hold, and nothing of the record found there is read after it,
 * since an `fc_unlink` of the file's last page may free the record in
 * between; the second hold looks the file up again. Linux's
 * `__filemap_add_folio` reads the mapping's entries only under the
 * `i_pages` lock, and after an allocation made with the lock let go takes
 * it again and checks them anew (mm/filemap.c:867-937, the allocations at
 * 926 and 935, the check again at 872-887).
 *
 * If the record describes an older version of the file, publish the new
 * record before giving way while draining the detached one.
 */
static int fc_insert(const struct fc_file *f, uint32_t pgoff, struct fc_page *p, uint32_t epoch)
{
    struct fc_page *old, **bucket;
    struct fc_inode *file, *spare = NULL, *stale = NULL;
    uint32_t steps = 0, flags = spin_lock_irqsave(&fc_lock);
    int need_spare;

    file = fc_find(f->mnt, f->ino);
    need_spare = !file || !fc_current(file, f);
    spin_unlock_irqrestore(&fc_lock, flags);
    if (need_spare) {
        spare = kzalloc(sizeof(*spare));
        if (!spare) {
            return 0;
        }
    }
    flags = spin_lock_irqsave(&fc_lock);
    if (epoch != *fc_changes(f->mnt, f->ino) ||
        *(const volatile int *)&f->mnt->state == VFS_UNMOUNTING) {
        spin_unlock_irqrestore(&fc_lock, flags);
        kfree(spare);
        return 0;
    }
    file = fc_find(f->mnt, f->ino);
    if (file && !fc_current(file, f)) {
        stale = file;
        fc_detach_file(stale);
        file = NULL;
    }
    bucket = fc_bucket(f->mnt, f->ino, pgoff);
    for (old = file ? *bucket : NULL; old; old = old->next) {
        if (old->file == file && old->pgoff == pgoff) {
            spin_unlock_irqrestore(&fc_lock, flags);
            kfree(spare);
            return 0;
        }
    }
    if (!file && !spare) {
        /* The record went or changed while the lock was out. */
        if (stale) {
            fc_drain_file(stale, &steps, &flags);
        }
        spin_unlock_irqrestore(&fc_lock, flags);
        return 0;
    }
    if (!file) {
        file = spare;
        spare = NULL;
        file->mnt = f->mnt;
        file->ino = f->ino;
        file->generation = f->generation;
        file->size = f->size;
        file->mtime = f->mtime;
        fc_link_file(file);
    }
    p->pgoff = pgoff;
    p->file = file;
    p->next = *bucket;
    *bucket = p;
    p->sibling_link = &file->pages;
    p->sibling = file->pages;
    if (p->sibling) {
        p->sibling->sibling_link = &p->sibling;
    }
    file->pages = p;
    fc_add_use(p);
    pages++;
    if (stale) {
        fc_drain_file(stale, &steps, &flags);
    }
    spin_unlock_irqrestore(&fc_lock, flags);
    kfree(spare);
    return 1;
}

/** Keep a copy of page `pgoff` of `f`, from the frame `phys` or, when it is
 * 0, from `buf`, as `fc_insert` keeps a page. */
static void fc_store(const struct fc_file *f, uint32_t pgoff, uintptr_t phys, const void *buf,
                     uint32_t epoch)
{
    struct fc_page *p;
    uint32_t flags;
    int copied;

    if (!buckets) {
        return;
    }
    flags = spin_lock_irqsave(&fc_lock);
    p = fc_new();
    spin_unlock_irqrestore(&fc_lock, flags);
    if (!p) {
        return;
    }
    copied = fc_copy(fc_phys(p), NULL, phys, buf);
    if (!copied || !fc_insert(f, pgoff, p, epoch)) {
        fc_free(p);
    }
}

/** Keep the frame `phys` itself, with a reference of the cache's, as page
 * `pgoff` of `f`, as `fc_insert` keeps a page. */
static void fc_keep(const struct fc_file *f, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    struct fc_page *p;
    uint32_t flags;

    if (!buckets || phys / PAGE_SIZE > FC_PFN_MASK) {
        return;
    }
    flags = spin_lock_irqsave(&fc_lock);
    p = fc_record();
    spin_unlock_irqrestore(&fc_lock, flags);
    if (!p) {
        return;
    }
    frame_ref(phys);
    p->pfn = (uint32_t)(phys / PAGE_SIZE);
    if (!fc_insert(f, pgoff, p, epoch)) {
        fc_free(p);
    }
}

/** Lend the frame of each page `first + i` of `f` that the cache has as the
 * file is now, its read ended, for each `phys[i]` that is 0, with a
 * reference; the count lent. */
static uint32_t fc_lend(const struct fc_file *f, uint32_t first, uint32_t count, uintptr_t *phys)
{
    struct fc_page *p;
    uint32_t i, lent = 0, flags = spin_lock_irqsave(&fc_lock);

    for (i = 0; i < count; i++) {
        if (phys[i] == 0 && (p = fc_lookup(f, first + i)) != NULL && !(p->pfn & FC_LOCK)) {
            phys[i] = fc_phys(p);
            frame_ref(phys[i]);
            fc_use(p);
            lent++;
        }
    }
    spin_unlock_irqrestore(&fc_lock, flags);
    return lent;
}

int filecache_read(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys)
{
    struct fc_file f;

    return pages && e->ofd && fc_file_of(e->ofd->vmnt, e->ofd->vnode, &f) &&
           fc_read(&f, pgoff, phys, NULL);
}

uint32_t filecache_lend(const struct fd_entry *e, uint32_t first, uint32_t count, uintptr_t *phys)
{
    struct fc_file f;

    return pages && e->ofd && fc_file_of(e->ofd->vmnt, e->ofd->vnode, &f)
               ? fc_lend(&f, first, count, phys)
               : 0;
}

int filecache_read_node(struct vfs_mount *mnt, void *node, uint32_t pgoff, void *buf)
{
    struct fc_file f;

    return pages && fc_file_of(mnt, node, &f) && fc_read(&f, pgoff, 0, buf);
}

/**
 * Read pages `first` to `first + count - 1` of the file open as `ofd` into
 * the frames `phys`, each filled whole, with zeros past the end of the
 * file: by the type's `read_frames`, which a type that has one fills them
 * with straight from the device, or a page at a time with `vfs_read`. The
 * pages filled, from the first; 0 at the end; or a negative errno.
 */
static int32_t fc_fill(struct open_file *ofd, uint32_t first, uint32_t count, const uintptr_t *phys)
{
    int32_t got = vfs_read_frames(ofd->vmnt, ofd->fhandle, first, count, phys, NULL);
    uint32_t i;
    uint8_t *page;

    if (got != -ENOSYS) {
        return got;
    }
    for (i = 0; i < count; i++) {
        page = kmap_temp(phys[i]);
        got = page ? vfs_read(ofd->vmnt, ofd->fhandle, page, PAGE_SIZE,
                              (uint64_t)(first + i) * PAGE_SIZE)
                   : -EIO;
        if (got >= 0 && got < (int32_t)PAGE_SIZE) {
            k_memset(page + got, 0, PAGE_SIZE - (uint32_t)got);
        }
        if (page) {
            kunmap_temp(page);
        }
        if (got <= 0) {
            break;
        }
        if (got < (int32_t)PAGE_SIZE) {
            i++;
            break;
        }
    }
    return i > 0 ? (int32_t)i : got;
}

/**
 * The size of the file open as `ofd` now, or `was` when it cannot be read,
 * which a read takes after its pages are up to date, as Linux's
 * `filemap_read` reads i_size after it has the folios (mm/filemap.c), so
 * that a truncation that raced the read ends the bytes it returns.
 */
static uint64_t fc_size_now(struct open_file *ofd, uint64_t was)
{
    struct vfs_stat st;

    return vfs_getattr(ofd->vmnt, ofd->vnode, &st) == 0 ? st.size : was;
}

/*
 * Readahead, Linux's on demand (mm/readahead.c). A read that misses a page
 * starts a window from it, as `page_cache_sync_ra` does, and marks the page
 * `async_size` pages before the window's end (FC_MARK); a read that reaches
 * a marked page starts the next window, as `page_cache_async_ra` does. Both
 * go in the reader's own context and neither waits for the disk: the
 * window's pages go into the cache locked (FC_LOCK), with frames of their
 * own, and the reader has the file system start their reads
 * (`vfs_read_frames` with a completion) and goes on, as Linux puts a
 * window's folios in the page cache locked and submits their bios
 * (`page_cache_ra_unbounded`, mm/readahead.c:208-300). The read's
 * completion, in the device's interrupt, clears the lock of each page or,
 * on a failure, drops it, and wakes the readers that wait for it, as
 * `folio_end_read` does; a reader that reaches a locked page waits on a
 * queue of a few that its page hashes to, as Linux waits on a folio's lock
 * on a hashed queue (`folio_wait_bit_common`, mm/filemap.c:1243-1370). The
 * reader that misses waits so for its own page, and reads it by itself
 * only when the window's read of it failed, as `filemap_read_folio` does
 * (mm/filemap.c:2442-2463).
 *
 * A page in flight holds no lock of its file and no reference to its
 * description or mount. A change of the file drops its pages, a locked one
 * among them, whose read then lands in a frame only its request holds, and
 * its completion finds no page to unlock; a free of the file's blocks waits
 * for the reads in flight over them (`block_settle`), as Linux's truncation
 * waits on each locked folio.
 */

/* No page to mark. */
#define FC_NO_MARK 0xFFFFFFFFu

/* The queues readers wait on for locked pages, which a page's file and
 * offset choose. A power of two. */
#define FC_WAITS 64u

/* Under `fc_lock`: what each window's completion needs, from the heap,
 * freed by the last of its calls. */
struct fc_flight {
    const struct vfs_mount *mnt;
    uint64_t ino;
    uint32_t start;   /* the page of the window's first frame */
    uint32_t pending; /* pages not yet ended, and one for the starter */
};

static wait_queue_t page_wq[FC_WAITS];

/** The queue a reader of page `pgoff` of the file `ino` of `mnt` waits on
 * while the page is locked. */
static wait_queue_t *fc_page_wq(const struct vfs_mount *mnt, uint64_t ino, uint32_t pgoff)
{
    return &page_wq[(fc_hash(mnt, ino) + pgoff) & (FC_WAITS - 1u)];
}

/** Whether page `pgoff` of `f` is in the cache as the file is now, read or
 * locked for its read, as Linux finds a folio in the page cache whether it
 * is up to date or not. Under `fc_lock`. */
static int fc_present(const struct fc_file *f, uint32_t pgoff)
{
    return fc_lookup(f, pgoff) != NULL;
}

/** The highest page of `f` from `index - max + 1` to `index` that is not
 * present (`fc_present`); `index - max` when every one is, or FC_NO_MARK
 * when every page from 0 to `index` is: Linux's `page_cache_prev_miss`
 * (mm/filemap.c:1758-1791). */
static uint32_t fc_prev_miss(const struct fc_file *f, uint32_t index, uint32_t max)
{
    uint32_t i, flags = spin_lock_irqsave(&fc_lock);

    for (i = 0; i < max; i++) {
        if (!fc_present(f, index - i)) {
            break;
        }
        if (index - i == 0) {
            spin_unlock_irqrestore(&fc_lock, flags);
            return FC_NO_MARK;
        }
    }
    spin_unlock_irqrestore(&fc_lock, flags);
    return index - i;
}

/** The lowest page of `f` from `index` to `index + max - 1` that is not
 * present; `index + max` when every one is, or 0 when the count wraps:
 * Linux's `page_cache_next_miss` (mm/filemap.c:1720-1756). */
static uint32_t fc_next_miss(const struct fc_file *f, uint32_t index, uint32_t max)
{
    uint32_t i, flags = spin_lock_irqsave(&fc_lock);

    for (i = 0; i < max; i++) {
        if (index + i < index) {
            i = 0u - index;
            break;
        }
        if (!fc_present(f, index + i)) {
            break;
        }
    }
    spin_unlock_irqrestore(&fc_lock, flags);
    return index + i;
}

/** The most pages a window of `ofd` takes for a read of `req` pages: the
 * description's `ra_pages`, or for a read longer than that as many as it
 * asks up to what one request to the device moves, as Linux's
 * `ractl_max_pages` allows a read up to `bdi->io_pages`
 * (mm/readahead.c:522-535). */
static uint32_t fc_max_pages(const struct open_file *ofd, uint32_t req)
{
    uint32_t max = ofd->ra.ra_pages ? ofd->ra.ra_pages : FILE_RA_PAGES, io = max;
    block_device_t *dev = vfs_mount_base(ofd->vmnt)->dev;

    if (dev) {
        io = block_max_sectors(dev) / (PAGE_SIZE / BLOCK_SECTOR_SIZE);
    }
    if (req > max && io > max) {
        max = req < io ? req : io;
    }
    return max;
}

/** The first window for a read of `size` pages: Linux's `get_init_ra_size`
 * (mm/readahead.c:361-373), four times the pages rounded up to a power of
 * two while those are at most a 32nd of `max`, twice them while at most a
 * quarter, and `max` past that. */
static uint32_t fc_init_size(uint32_t size, uint32_t max)
{
    uint32_t round = 1u;

    while (round < size && round < 0x80000000u) {
        round *= 2u;
    }
    if (round <= max / 32u) {
        return round * 4u;
    }
    return round <= max / 4u ? round * 2u : max;
}

/** The window after one of `cur` pages: Linux's `get_next_ra_size`
 * (mm/readahead.c:379-389). */
static uint32_t fc_next_size(uint32_t cur, uint32_t max)
{
    if (cur < max / 16u) {
        return 4u * cur;
    }
    return cur <= max / 2u ? 2u * cur : max;
}

/* The most pages one call of the type's `read_frames` fills: what one
 * request to a device moves, so that a long window goes to the disk in
 * requests as long as Linux's (`ractl_max_pages`). */
#define FC_RUN_MOST (BLOCK_REQ_MAX_SECTORS / (PAGE_SIZE / BLOCK_SECTOR_SIZE))

/* The records and frames of a run of pages read in one call: FC_WINDOW on
 * the stack, and for a longer run arrays from the heap. */
struct fc_run {
    struct fc_page **recs;
    uintptr_t *phys;
    uint32_t room;
    struct fc_page *recs_small[FC_WINDOW];
    uintptr_t phys_small[FC_WINDOW];
};

/** Give `r` room for `want` pages, up to FC_RUN_MOST, from the heap past
 * FC_WINDOW, or FC_WINDOW when the heap has none. Not under `fc_lock`. */
static void fc_run_init(struct fc_run *r, uint32_t want)
{
    r->recs = r->recs_small;
    r->phys = r->phys_small;
    r->room = FC_WINDOW;
    if (want <= FC_WINDOW) {
        return;
    }
    want = want < FC_RUN_MOST ? want : FC_RUN_MOST;
    r->recs = kmalloc(want * (sizeof(*r->recs) + sizeof(*r->phys)));
    if (!r->recs) {
        r->recs = r->recs_small;
        return;
    }
    r->phys = (uintptr_t *)(void *)(r->recs + want);
    r->room = want;
}

/** Give back what `fc_run_init` took. */
static void fc_run_free(struct fc_run *r)
{
    if (r->recs != r->recs_small) {
        kfree(r->recs);
    }
}

/**
 * The completion of the read of `count` pages of the window `arg`, a
 * `struct fc_flight`, from its page `index`, into `frames`, as
 * `block_frames_fn` calls it: each page still in the cache with that frame
 * and locked is unlocked, or dropped when `status` is an error, as
 * `folio_end_read` marks a folio up to date or not and unlocks it, and the
 * readers waiting on their pages' queues are woken. The call that ends the
 * window's last page, after the starter has let go (`fc_flight_put`),
 * frees the window. The window is read only under `fc_lock`, before the
 * call gives up its pages: once they are given up, the starter's put or
 * another call on another processor may free it, so the wakes use copies.
 * Any context; takes `fc_lock` and the queues' locks.
 */
static void fc_done(void *arg, uint32_t index, const uintptr_t *frames, uint32_t count, int status)
{
    struct fc_flight *w = arg;
    const struct vfs_mount *mnt;
    struct fc_page *p;
    uint64_t ino;
    uint32_t i, left, first, flags = spin_lock_irqsave(&fc_lock);

    for (i = 0; i < count; i++) {
        for (p = *fc_bucket(w->mnt, w->ino, w->start + index + i); p; p = p->next) {
            if (p->pgoff == w->start + index + i && p->file->link && p->file->mnt == w->mnt &&
                p->file->ino == w->ino) {
                break;
            }
        }
        if (!p || !(p->pfn & FC_LOCK) || fc_phys(p) != frames[i]) {
            continue;
        }
        if (status == 0) {
            p->pfn &= ~FC_LOCK;
        } else {
            fc_drop(p);
        }
    }
    mnt = w->mnt;
    ino = w->ino;
    first = w->start + index;
    w->pending -= count;
    left = w->pending;
    spin_unlock_irqrestore(&fc_lock, flags);
    for (i = 0; i < count && i < FC_WAITS; i++) {
        wq_wake_all(fc_page_wq(mnt, ino, first + i));
    }
    if (left == 0) {
        kfree(w);
    }
}

/** Let go of the starter's hold of the window `w`, which keeps it while
 * its pages are started, and free it when its pages have all ended. */
static void fc_flight_put(struct fc_flight *w)
{
    uint32_t left, flags = spin_lock_irqsave(&fc_lock);

    left = --w->pending;
    spin_unlock_irqrestore(&fc_lock, flags);
    if (left == 0) {
        kfree(w);
    }
}

/**
 * Put pages `at` onward of `f` that the cache lacks, up to `n` and to the
 * first it has, into the cache locked, `mark` among them marked, each with
 * a new frame whose number `r` holds and an extra reference of the
 * caller's, so that a drop before the read starts does not free the
 * frame. `epoch` is as `fc_insert` takes it. The pages put in, from `at`.
 * Every frame gets the caller's reference before the first insertion, so
 * that each frame left over after an insertion fails holds the two that
 * its cleanup drops.
 */
static uint32_t fc_lock_run(const struct fc_file *f, uint32_t at, uint32_t n, uint32_t mark,
                            uint32_t epoch, struct fc_run *r)
{
    uint32_t got = 0, i, flags = spin_lock_irqsave(&fc_lock);

    while (got < n && got < r->room && !fc_present(f, at + got) &&
           (r->recs[got] = fc_new()) != NULL) {
        r->phys[got] = fc_phys(r->recs[got]);
        r->recs[got]->pfn |= FC_LOCK | (at + got == mark ? FC_MARK : 0u);
        got++;
    }
    spin_unlock_irqrestore(&fc_lock, flags);
    for (i = 0; i < got; i++) {
        frame_ref(r->phys[i]);
    }
    for (i = 0; i < got; i++) {
        if (!fc_insert(f, at + i, r->recs[i], epoch)) {
            break;
        }
    }
    n = i;
    for (; i < got; i++) {
        free_frame(r->phys[i]);
        fc_free(r->recs[i]);
    }
    return n;
}

/**
 * Start the reads of the `count` pages of `f`, open as `ofd`, from `start`,
 * cut at the file's end, that the cache lacks, into pages put in the cache
 * locked (`fc_lock_run`), `mark` marked when it is among them, and return
 * without waiting for the disk: `fc_done` ends each page, a page the file
 * system could not start among them. `epoch` is `fc_changes` taken before.
 * A page the cache has ends a run of pages, and the window goes on after
 * it; a page that cannot be had ends the window. The caller holds the
 * description and no spinlock, and may sleep, for the file system's lock
 * and for room among the requests.
 */
static void fc_read_async(struct open_file *ofd, const struct fc_file *f, uint32_t start,
                          uint32_t count, uint32_t mark, uint32_t epoch)
{
    uint64_t span = (f->size + PAGE_SIZE - 1u) / PAGE_SIZE;
    uint32_t at = start, end, n, i, flags;
    struct block_frames_done done;
    struct fc_flight *w;
    struct fc_run r;
    int32_t got;

    if (!buckets || count == 0 || start >= span) {
        return;
    }
    end = start + (count < span - start ? count : (uint32_t)(span - start));
    fc_run_init(&r, end - start);
    while (at < end) {
        flags = spin_lock_irqsave(&fc_lock);
        while (at < end && fc_present(f, at)) {
            at++;
        }
        spin_unlock_irqrestore(&fc_lock, flags);
        w = at < end ? kmalloc(sizeof(*w)) : NULL;
        n = w ? fc_lock_run(f, at, end - at, mark, epoch, &r) : 0;
        if (n == 0) {
            kfree(w);
            break;
        }
        w->mnt = f->mnt;
        w->ino = f->ino;
        w->start = at;
        w->pending = n + 1u;
        done.fn = fc_done;
        done.arg = w;
        done.index = 0;
        got = vfs_read_frames(ofd->vmnt, ofd->fhandle, at, n, r.phys, &done);
        got = got < 0 ? 0 : got;
        if ((uint32_t)got < n) {
            fc_done(w, (uint32_t)got, r.phys + got, n - (uint32_t)got, -EIO);
        }
        for (i = 0; i < n; i++) {
            free_frame(r.phys[i]); /* the starter's reference */
        }
        fc_flight_put(w);
        if ((uint32_t)got < n) {
            break;
        }
        at += n;
    }
    fc_run_free(&r);
}

/**
 * The window a read of `ofd` that misses page `index` reads at once, from
 * `index`, when the read wants `req` pages from there, with `*mark` the page
 * to mark or FC_NO_MARK: Linux's `page_cache_sync_ra` (mm/readahead.c:
 * 537-608). Under POSIX_FADV_RANDOM, what the read asks for up to the larger
 * of a request and `ra_pages` (`force_page_cache_ra`). At the start of the
 * file, for a read longer than the most a window takes, or one that goes on
 * from where the last read ended, the first window (`fc_init_size`), whose
 * pages past those asked for, or half of it, start the next. Otherwise the
 * pages the cache has just before `index` tell a stream from a random read:
 * a run no longer than the read is a random read, which reads what it asks
 * for and leaves the state alone; a longer one sets a window of the run and
 * the read, twice the run when it reaches the file's start, whose last page
 * starts the next.
 */
static uint32_t fc_sync_ra(struct open_file *ofd, const struct fc_file *f, uint32_t index,
                           uint32_t req, uint32_t *mark)
{
    struct file_ra_state *ra = &ofd->ra;
    uint32_t max, miss, contig;

    *mark = FC_NO_MARK;
    if (ra->random) {
        max = fc_max_pages(ofd, 0xFFFFFFFFu);
        return req < max ? req : max;
    }
    max = fc_max_pages(ofd, req);
    if (index == 0 || req > max || (uint64_t)index - ra->prev_pos / PAGE_SIZE <= 1u) {
        ra->start = index;
        ra->size = fc_init_size(req, max);
        ra->async_size = ra->size > req ? ra->size - req : ra->size >> 1;
    } else {
        miss = fc_prev_miss(f, index - 1u, max);
        contig = index - miss - 1u;
        if (contig <= req) {
            return req;
        }
        if (miss == FC_NO_MARK) {
            contig *= 2u;
        }
        ra->start = index;
        ra->size = contig + req < max ? contig + req : max;
        ra->async_size = 1;
    }
    *mark = ra->start + ra->size - ra->async_size;
    return ra->size;
}

/**
 * Start the window after a marked page, `index`, that a read of `ofd`
 * wanting `req` pages from there reached, without waiting for it
 * (`fc_read_async`): Linux's `page_cache_async_ra`
 * (mm/readahead.c:611-674). The mark where the last window put it pushes
 * the window on and grows it (`fc_next_size`); a mark the description did
 * not set, as another stream's, starts a window at the first page from
 * `index` that is not present, of the pages between and the read, grown.
 * The whole of the new window starts the next, so its first page is
 * marked.
 */
static void fc_async_ra(struct open_file *ofd, const struct fc_file *f, uint32_t index,
                        uint32_t req)
{
    struct file_ra_state *ra = &ofd->ra;
    uint32_t max = fc_max_pages(ofd, req), start, next;

    if (index == ra->start + ra->size - ra->async_size) {
        ra->start += ra->size;
        next = fc_next_size(ra->size, max);
        ra->size = next > ra->size ? next : ra->size;
    } else {
        start = fc_next_miss(f, index + 1u, max);
        if (start == 0 || start - index > max) {
            return;
        }
        ra->start = start;
        ra->size = fc_next_size(start - index + req, max);
    }
    ra->async_size = ra->size;
    fc_read_async(ofd, f, ra->start, ra->size, ra->start, *fc_changes(f->mnt, f->ino));
}

/** Read `left` bytes at `at` of the file open as `ofd`, which stay inside
 * one page, straight into the user buffer `dst`, under the big lock, since
 * the file system then writes the user's buffer itself; the type's answer. */
static int32_t fc_read_direct(struct open_file *ofd, uint64_t at, char __user *dst, uint32_t left)
{
    int big = !bkl_held();
    int32_t rc;

    if (big) {
        bkl_enter();
    }
    rc = vfs_read(ofd->vmnt, ofd->fhandle, (void __force *)dst, left, at);
    if (big) {
        bkl_exit();
    }
    return rc;
}

/* A reader's wait for a locked page: the file, the page and its frame. */
struct fc_wait {
    const struct fc_file *f;
    uint32_t pgoff;
    uint32_t pfn; /* the frame's number, as `fc_page.pfn` holds it */
};

/** Whether the page a reader waits for, `arg` a `struct fc_wait`, is no
 * longer locked with the frame it was locked with: read, dropped or put
 * back with another frame. */
static int fc_unlocked(void *arg)
{
    const struct fc_wait *w = arg;
    struct fc_page *p;
    uint32_t flags = spin_lock_irqsave(&fc_lock);
    int done;

    p = fc_lookup(w->f, w->pgoff);
    done = !p || !(p->pfn & FC_LOCK) || (p->pfn & FC_PFN_MASK) != w->pfn;
    spin_unlock_irqrestore(&fc_lock, flags);
    return done;
}

/**
 * Find page `pgoff` of `f`, open as `ofd`, for a read that wants pages up
 * to `last`: the frame of the page in the cache, with a reference for the
 * caller, after a wait while it is locked for its read (`fc_unlocked`); or,
 * for a page the cache lacks, a window started from it (`fc_sync_ra`,
 * `fc_read_async`) and the page then waited for, as Linux's
 * `filemap_get_pages` starts `page_cache_sync_ra` and finds the folio
 * again (mm/filemap.c:2560-2610); or, when that window's read of the page
 * failed or the page went, the frame of a new record, which `*p` receives,
 * for the caller to read the page into itself; or 0 with no frame to be
 * had. A marked page reached, read or locked, starts the next window
 * (`fc_async_ra`) before the page is had or waited for, as
 * `filemap_get_pages` starts it before it waits on the folio's lock
 * (mm/filemap.c:2584-2593).
 */
static uintptr_t fc_get(struct open_file *ofd, const struct fc_file *f, uint32_t pgoff,
                        uint32_t last, struct fc_page **p)
{
    struct fc_wait w;
    struct fc_page *hit;
    uintptr_t phys = 0;
    uint32_t flags, nr, mark, epoch;
    int marked, locked, tried = 0;

    w.f = f;
    w.pgoff = pgoff;
    for (;;) {
        marked = locked = 0;
        flags = spin_lock_irqsave(&fc_lock);
        hit = pages ? fc_lookup(f, pgoff) : NULL;
        if (hit) {
            marked = (hit->pfn & FC_MARK) != 0;
            hit->pfn &= ~FC_MARK;
            locked = (hit->pfn & FC_LOCK) != 0;
            w.pfn = hit->pfn & FC_PFN_MASK;
            if (!locked) {
                phys = fc_phys(hit);
                frame_ref(phys);
                fc_use(hit);
            }
        } else if (tried) {
            *p = fc_new();
            phys = *p ? fc_phys(*p) : 0;
        }
        spin_unlock_irqrestore(&fc_lock, flags);
        if (marked) {
            fc_async_ra(ofd, f, pgoff, last - pgoff + 1u);
        }
        if ((hit && !locked) || (!hit && tried)) {
            return phys;
        }
        if (!hit) {
            epoch = *fc_changes(f->mnt, f->ino);
            nr = fc_sync_ra(ofd, f, pgoff, last - pgoff + 1u, &mark);
            fc_read_async(ofd, f, pgoff, nr, mark, epoch);
        } else {
            (void)wq_wait_event_nointr(fc_page_wq(f->mnt, f->ino, pgoff), fc_unlocked, &w);
        }
        tried = 1;
    }
}

/**
 * Read page `pgoff` of `f`, open as `ofd`, into the frame of `p`, which
 * `fc_new` gave, in the call (`fc_fill`), for a reader whose window did not
 * bring it. The bytes of `pgoff` the file has after the read
 * (`fc_size_now`), which stop short of a page only at its end; 0 past the
 * end; or a negative errno.
 */
static int32_t fc_read_one(struct open_file *ofd, const struct fc_file *f, uint32_t pgoff,
                           struct fc_page *p)
{
    uintptr_t phys = fc_phys(p);
    uint64_t end = (uint64_t)pgoff * PAGE_SIZE, size;
    int32_t got = fc_fill(ofd, pgoff, 1, &phys);

    if (got <= 0) {
        return got;
    }
    size = fc_size_now(ofd, f->size);
    if (size <= end) {
        return 0;
    }
    return size - end < PAGE_SIZE ? (int32_t)(size - end) : (int32_t)PAGE_SIZE;
}

/**
 * Read up to `left` bytes at `at` of `f`, open as `ofd`, inside one page,
 * into the user buffer `dst`, for a read that wants pages up to `last`:
 * from the cache's frame, with a reference taken so that a drop meanwhile
 * does not free it (`fc_get`), or into a new frame read from the file
 * (`fc_read_one`), which then joins the cache. With no frame to be had the
 * file system reads the page's part into `dst` itself (`fc_read_direct`).
 * The bytes read, which stop short of the page's end or of `left` only at
 * the end of the file, as its size reads once the page is had
 * (`fc_size_now`); -EFAULT; or the file system's error.
 */
static int32_t fc_read_page(struct open_file *ofd, const struct fc_file *f, uint64_t at,
                            char __user *dst, uint32_t left, uint32_t last)
{
    uint32_t pgoff = (uint32_t)(at / PAGE_SIZE), in = (uint32_t)(at % PAGE_SIZE);
    uint32_t take = PAGE_SIZE - in < left ? PAGE_SIZE - in : left, epoch = 0;
    struct fc_page *p = NULL;
    uintptr_t phys = fc_get(ofd, f, pgoff, last, &p);
    int32_t got = PAGE_SIZE, rc = 0;
    uint8_t *page;

    page = phys ? kmap_temp(phys) : NULL;
    if (!page) {
        if (p) {
            fc_free(p);
        } else if (phys) {
            free_frame(phys);
        }
        return fc_read_direct(ofd, at, dst, take);
    }
    if (p) {
        epoch = *fc_changes(f->mnt, f->ino);
        got = fc_read_one(ofd, f, pgoff, p);
        if (got < 0) {
            rc = got;
        } else if ((uint32_t)got <= in) {
            take = 0;
        } else if (take > (uint32_t)got - in) {
            take = (uint32_t)got - in;
        }
    } else {
        uint64_t size = fc_size_now(ofd, f->size);

        if (size <= at) {
            take = 0;
        } else if (take > size - at) {
            take = (uint32_t)(size - at);
        }
    }
    if (rc == 0 && take > 0 && copy_to_user(dst, page + in, take) != 0) {
        rc = -EFAULT;
    }
    kunmap_temp(page);
    if (!p) {
        free_frame(phys);
    } else if (got <= 0 || !fc_insert(f, pgoff, p, epoch)) {
        fc_free(p);
    }
    return rc != 0 ? rc : (int32_t)take;
}

int filecache_file_read(struct open_file *ofd, char __user *buf, uint32_t count, uint64_t offset,
                        int32_t *got, int (*cut)(const struct open_file *ofd))
{
    struct fc_file f;
    uint32_t done = 0, last;
    int32_t n = 0;

    if (!buckets || count == 0 || !fc_file_of(ofd->vmnt, ofd->vnode, &f)) {
        return 0;
    }
    if (offset >= f.size) {
        *got = 0;
        return 1;
    }
    if ((f.size - 1u) / PAGE_SIZE > 0xFFFFFFFFu) {
        return 0;
    }
    if (count > f.size - offset) {
        count = (uint32_t)(f.size - offset);
    }
    last = (uint32_t)((offset + count - 1u) / PAGE_SIZE);
    while (done < count) {
        uint32_t in = (uint32_t)((offset + done) % PAGE_SIZE);
        uint32_t want = PAGE_SIZE - in < count - done ? PAGE_SIZE - in : count - done;

        if (done > 0 && cut && cut(ofd)) {
            break;
        }
        n = fc_read_page(ofd, &f, offset + done, buf + done, count - done, last);
        if (n <= 0) {
            break;
        }
        done += (uint32_t)n;
        if ((uint32_t)n < want) {
            break;
        }
    }
    if (done > 0) {
        ofd->ra.prev_pos = offset + done;
    }
    *got = done > 0 || n > 0 ? (int32_t)done : n;
    return 1;
}

/** The last byte of the `len` bytes at `offset`, or 2^63 - 1 for a `len`
 * of 0, which is the rest of the file, and for a range that wraps, as
 * Linux's `generic_fadvise` reads them (mm/fadvise.c:66-74). */
static uint64_t fc_endbyte(uint64_t offset, uint64_t len)
{
    return len == 0 || offset + len < len ? 0x7FFFFFFFFFFFFFFFull : offset + len - 1u;
}

void filecache_willneed(struct open_file *ofd, uint64_t offset, uint64_t len)
{
    struct fc_file f;
    uint64_t first = offset / PAGE_SIZE, want;
    uint32_t max;

    if (!buckets || !ofd->vmnt || !fc_file_of(ofd->vmnt, ofd->vnode, &f) || first > 0xFFFFFFFFu) {
        return;
    }
    max = fc_max_pages(ofd, 0xFFFFFFFFu);
    want = fc_endbyte(offset, len) / PAGE_SIZE - first + 1u;
    fc_read_async(ofd, &f, (uint32_t)first, want < max ? (uint32_t)want : max, FC_NO_MARK,
                  *fc_changes(f.mnt, f.ino));
}

void filecache_dontneed(struct open_file *ofd, uint64_t offset, uint64_t len)
{
    struct fc_inode *file;
    struct fc_file f;
    uint64_t endbyte = fc_endbyte(offset, len), first, final;
    uint32_t flags;

    if (!buckets || !ofd->vmnt || !fc_file_of(ofd->vmnt, ofd->vnode, &f) || f.size == 0) {
        return;
    }
    first = (offset + PAGE_SIZE - 1u) / PAGE_SIZE;
    final = endbyte / PAGE_SIZE;
    if ((endbyte & (PAGE_SIZE - 1u)) != PAGE_SIZE - 1u && endbyte != f.size - 1u) {
        if (final == 0) {
            return;
        }
        final--;
    }
    if (final < first) {
        return;
    }
    flags = spin_lock_irqsave(&fc_lock);
    file = pages ? fc_find(f.mnt, f.ino) : NULL;
    if (file) {
        fc_drop_range(file, first, final, &flags);
    }
    spin_unlock_irqrestore(&fc_lock, flags);
}

uint32_t filecache_epoch(struct vfs_mount *mnt, void *node)
{
    struct fc_file f;

    return fc_file_of(mnt, node, &f) ? *fc_changes(f.mnt, f.ino) : 0;
}

void filecache_store(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    struct fc_file f;

    if (e->ofd && fc_file_of(e->ofd->vmnt, e->ofd->vnode, &f)) {
        fc_store(&f, pgoff, phys, NULL, epoch);
    }
}

void filecache_keep(const struct fd_entry *e, uint32_t pgoff, uintptr_t phys, uint32_t epoch)
{
    struct fc_file f;

    if (e->ofd && fc_file_of(e->ofd->vmnt, e->ofd->vnode, &f)) {
        fc_keep(&f, pgoff, phys, epoch);
    }
}

int filecache_keeps(const struct fd_entry *e)
{
    struct fc_file f;

    return e->ofd && fc_file_of(e->ofd->vmnt, e->ofd->vnode, &f);
}

void filecache_store_node(struct vfs_mount *mnt, void *node, uint32_t pgoff, const void *buf,
                          uint32_t epoch)
{
    struct fc_file f;

    if (fc_file_of(mnt, node, &f)) {
        fc_store(&f, pgoff, 0, buf, epoch);
    }
}

void filecache_forget(struct vfs_mount *mnt, void *node)
{
    struct fc_inode *file;
    struct fc_file f;
    uint32_t steps = 0, flags;

    if (!fc_file_of(mnt, node, &f)) {
        return;
    }
    flags = spin_lock_irqsave(&fc_lock);
    (*fc_changes(f.mnt, f.ino))++;
    file = pages ? fc_find(f.mnt, f.ino) : NULL;
    if (file) {
        fc_drop_file(file, &steps, &flags);
    }
    spin_unlock_irqrestore(&fc_lock, flags);
}

void filecache_before(struct vfs_mount *mnt, void *node, struct filecache_was *was)
{
    struct fc_file f;

    was->known = fc_file_of(mnt, node, &f);
    was->size = was->known ? f.size : 0;
    was->mtime = was->known ? f.mtime : 0;
    was->generation = was->known ? f.generation : 0;
}

void filecache_changed(struct vfs_mount *mnt, void *node, const struct filecache_was *was,
                       uint64_t from, uint64_t to)
{
    struct fc_inode *file;
    struct fc_file f;
    uint64_t first = from / PAGE_SIZE, last = to > from ? (to - 1u) / PAGE_SIZE : first;
    uint32_t steps = 0, flags;

    if (!was->known || !fc_file_of(mnt, node, &f)) {
        filecache_forget(mnt, node);
        return;
    }
    flags = spin_lock_irqsave(&fc_lock);
    (*fc_changes(f.mnt, f.ino))++;
    file = pages ? fc_find(f.mnt, f.ino) : NULL;
    if (file && (file->generation != was->generation || file->size != was->size ||
                 file->mtime != was->mtime)) {
        fc_drop_file(file, &steps, &flags);
        spin_unlock_irqrestore(&fc_lock, flags);
        return;
    }
    if (file) {
        fc_drop_range(file, first, last, &flags);
    }
    file = pages ? fc_find(f.mnt, f.ino) : NULL;
    if (file) {
        file->size = f.size;
        file->mtime = f.mtime;
    }
    spin_unlock_irqrestore(&fc_lock, flags);
}

void filecache_forget_ino(const struct vfs_mount *mnt, uint64_t ino)
{
    struct fc_inode *file;
    uint32_t steps = 0, flags;

    if (!buckets) {
        return;
    }
    mnt = vfs_mount_base(mnt);
    flags = spin_lock_irqsave(&fc_lock);
    (*fc_changes(mnt, ino))++;
    file = pages ? fc_find(mnt, ino) : NULL;
    if (file) {
        fc_drop_file(file, &steps, &flags);
    }
    spin_unlock_irqrestore(&fc_lock, flags);
}

void filecache_forget_mount(const struct vfs_mount *mnt)
{
    uint32_t i, steps = 0, flags = spin_lock_irqsave(&fc_lock);

    mnt = vfs_mount_base(mnt);
    for (i = 0; i < FC_EPOCHS; i++) {
        changes[i]++;
    }
    while (mnt->filecache_files) {
        fc_drop_file(mnt->filecache_files, &steps, &flags);
    }
    spin_unlock_irqrestore(&fc_lock, flags);
}

uint32_t filecache_pages(void)
{
    return pages;
}

uint32_t filecache_shrink(uint32_t want)
{
    uint32_t given = 0, flags = spin_lock_irqsave(&fc_lock);

    while (oldest && given < want) {
        given += (uint32_t)fc_drop(oldest);
    }
    spin_unlock_irqrestore(&fc_lock, flags);
    return given;
}
