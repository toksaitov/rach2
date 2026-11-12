/*
 * filecache.c: the pages of ext2 files that read(2), private mappings and
 * exec read, kept between them, as filecache.h describes them.
 *
 * The buffer cache, held to half the kernel's heap, keeps at most a gigabyte
 * of a machine of 16 GiB, so a file read again, a program started again or a
 * library mapped again would come from the disk again. This cache keeps a
 * copy of each page read any of those ways in a free frame, found by its
 * file's mount, inode number and generation, and checked against the size
 * and the modification time the file has when the page is asked for. A
 * write, a truncation and an allocation through the VFS drop the file's pages
 * after they change it, and an unmount every page of its files; each counts
 * as a change of the file, and a page read while a change of its file ran is
 * not kept, since the read may have seen part of it. A private mapping gets
 * a copy of a cached page. The pages in memory of a disk file that no shared
 * mapping through a writable descriptor maps share the cache's frame, as
 * Linux's page cache gives read(2) and every mapping one folio: the file's
 * pages change only as a write or a truncation changes the file, which
 * drops its pages here after, and such a mapping, whose stores reach the
 * pages before the file, drops them when it is made. Each file that has
 * pages has a record that lists them and holds the size and the modification
 * time they were read at, so a change drops that file's pages without a
 * search of every other file's. The frames are the machine's free memory:
 * the cache takes a new one only while a quarter of the frames are free and
 * its records fit in an eighth of the heap, and past either uses the frame
 * of its least recently used page; it gives frames back, the least recently
 * used first, when memory runs short (`mem_reclaim`).
 *
 * The table of the pages and the table of the files each have a list for
 * every four to eight frames the machine has, a power of two set at boot, so
 * that a list stays a few entries long when the cache holds all the frames it
 * may take.
 *
 * `fc_lock`, a spinlock taken with interrupts off, covers both tables, the
 * use order, the count and the counters of changes; the copies of pages and
 * the reads of files run outside it, the frame of a page being read held by
 * a reference. It comes after the page-table locks and before the temporary
 * kernel slots, whose `kmap_temp` a hit copies under it, as Linux takes the
 * `i_pages` lock under the page-table lock (mm/filemap.c:89-92), and no
 * caller needs the big lock.
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
#include "../lib/kirq.h"
#include "../proc/process.h"
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

/* A file that has pages in the cache, the list of those pages, so that a
 * change to the file drops them without a search of the other files' pages,
 * and what the file was when they were read. The record lives while the
 * file has a page. */
struct fc_inode {
    struct fc_inode *next, **link; /* the slot's list, and what points here */
    const struct vfs_mount *mnt;
    uint64_t ino;
    uint64_t size;
    int64_t mtime;
    uint32_t generation;
    struct fc_page *pages;
};

/* A cached page: which page of which file and the frame of the copy. The
 * record is kept to 64 bytes on x86-64 and 32 on i686, a block of the
 * heap's class of that size, since the cache may hold millions. */
struct fc_page {
    struct fc_page *next;                    /* the bucket's list */
    struct fc_page *sibling, **sibling_link; /* the file's list */
    struct fc_page *older, *younger;         /* the use order */
    struct fc_inode *file;
    uint32_t pgoff;
    /* The frame's number, its address over PAGE_SIZE, and FC_MARK. */
    uint32_t pfn;
};

/* The bit of `fc_page.pfn` that marks the page whose read starts the next
 * readahead window, Linux's PG_readahead; the bits below it are the frame's
 * number, so a frame at or past 8 TiB is not kept. */
#define FC_MARK     0x80000000u
#define FC_PFN_MASK 0x7FFFFFFFu

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
        if (p->pgoff == pgoff && p->file->mnt == f->mnt && p->file->ino == f->ino) {
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

/** Take `p` off every list, and free its file's record when it was the
 * file's last page; the record of `p` and its frame are the caller's. */
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
    if (!file->pages) {
        *file->link = file->next;
        if (file->next) {
            file->next->link = file->link;
        }
        kfree(file);
    }
}

/** Free `p`, and its file's record when it was the file's last page; 1 when
 * its frame went back to the pool, 0 when a reader still holds it. */
static int fc_drop(struct fc_page *p)
{
    uint32_t left;

    fc_unlink(p);
    left = frame_unref(fc_phys(p));
    kfree(p);
    return left == 0;
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
    uint32_t flags = spin_lock_irqsave(&fc_lock);
    int hit = 0;

    p = fc_lookup(f, pgoff);
    if (p) {
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
 */
static int fc_insert(const struct fc_file *f, uint32_t pgoff, struct fc_page *p, uint32_t epoch)
{
    struct fc_page *old, **bucket;
    struct fc_inode *file, *spare = NULL;
    uint32_t flags = spin_lock_irqsave(&fc_lock);
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
    if (epoch != *fc_changes(f->mnt, f->ino)) {
        spin_unlock_irqrestore(&fc_lock, flags);
        kfree(spare);
        return 0;
    }
    file = fc_find(f->mnt, f->ino);
    if (file && !fc_current(file, f)) {
        fc_drop_file(file);
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
        /* The file's record went since the look without the lock. */
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
        file->link = &files[fc_slot(f->mnt, f->ino)];
        file->next = *file->link;
        if (file->next) {
            file->next->link = &file->next;
        }
        *file->link = file;
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
 * file is now, for each `phys[i]` that is 0, with a reference; the count
 * lent. */
static uint32_t fc_lend(const struct fc_file *f, uint32_t first, uint32_t count, uintptr_t *phys)
{
    struct fc_page *p;
    uint32_t i, lent = 0, flags = spin_lock_irqsave(&fc_lock);

    for (i = 0; i < count; i++) {
        if (phys[i] == 0 && (p = fc_lookup(f, first + i)) != NULL) {
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
    int32_t got = vfs_read_frames(ofd->vmnt, ofd->fhandle, first, count, phys);
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
 * reads a window from it at once, as `page_cache_sync_ra` does, and marks
 * the page `async_size` pages before the window's end (FC_MARK); a read
 * that reaches a marked page queues the next window, as
 * `page_cache_async_ra` does, and goes on without waiting for it. A queued
 * window is a job, which a worker thread reads into the cache while the
 * reader takes the pages before it; Linux puts the window's folios in the
 * page cache locked and has the device fill them, and a reader that reaches
 * one waits on its lock. Here a reader that reaches a page a job will read
 * waits for the job's end instead (`fc_job_at`), and the window's pages
 * join the cache as they are read.
 */

/* The jobs that may be queued or read at once, and the workers that read
 * them, each a kernel thread made when a job finds no idle one. */
#define FC_JOBS    16u
#define FC_WORKERS 4u

/* No page to mark. */
#define FC_NO_MARK 0xFFFFFFFFu

/* A window queued for a worker. The job holds its description and has
 * entered its mount, so that an unmount waits for its end (`vfs_enter`). */
struct fc_job {
    struct open_file *ofd; /* NULL for a free slot */
    struct fc_file f;
    uint32_t start, count; /* the window's pages */
    uint32_t mark;         /* the page marked when read, or FC_NO_MARK */
    uint32_t epoch;        /* `fc_changes` when it was queued */
    /* The order of queueing, never 0, by which a waiter knows its job;
     * 0 once the job has ended. */
    uint32_t serial;
    int running;
};

/* Under `fc_lock`. */
static struct fc_job jobs[FC_JOBS];
static uint32_t job_serials;
static uint32_t workers, workers_busy;
/* Workers sleep on `work_wq` for a job, readers on `done_wq` for one's end. */
static wait_queue_t work_wq, done_wq;

/** The slot of the job that will read page `pgoff` of `f`, or -1. Under
 * `fc_lock`. */
static int fc_job_at(const struct fc_file *f, uint32_t pgoff)
{
    uint32_t i;

    for (i = 0; i < FC_JOBS; i++) {
        if (jobs[i].ofd && jobs[i].f.mnt == f->mnt && jobs[i].f.ino == f->ino &&
            pgoff - jobs[i].start < jobs[i].count) {
            return (int)i;
        }
    }
    return -1;
}

/** Whether page `pgoff` of `f` is in the cache as the file is now or will
 * be read by a job, as Linux finds a folio in the page cache whether it is
 * up to date or locked for its read. Under `fc_lock`. */
static int fc_present(const struct fc_file *f, uint32_t pgoff)
{
    return fc_lookup(f, pgoff) != NULL || fc_job_at(f, pgoff) >= 0;
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

/** Whether the job a reader waits for has ended. */
static int fc_job_ended(void *arg)
{
    const uint32_t *w = arg; /* the slot, then the job's serial */

    return *(volatile uint32_t *)&jobs[w[0]].serial != w[1];
}

/** Whether a job waits for a worker. */
static int fc_job_waiting(void *arg)
{
    uint32_t i;

    (void)arg;
    for (i = 0; i < FC_JOBS; i++) {
        if (*(volatile uint32_t *)&jobs[i].serial != 0 && !*(volatile int *)&jobs[i].running) {
            return 1;
        }
    }
    return 0;
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
 * Read the pages of job `j` that the cache lacks into new frames, which
 * join the cache, a run of at most FC_RUN_MOST pages a call of the type's
 * `read_frames` (`fc_fill`), as Linux's `page_cache_ra_unbounded` skips
 * the folios the page cache has (mm/readahead.c:236-266). The page `j->mark`
 * names takes the mark when it is read, unless a reader reached it first
 * and took the mark from the job. A failure or the file's end ends the job.
 */
static void fc_job_read(struct fc_job *j)
{
    struct fc_run r;
    uint32_t at = j->start, end = j->start + j->count, n, i, flags;
    int32_t got;

    fc_run_init(&r, j->count);
    while (at < end) {
        flags = spin_lock_irqsave(&fc_lock);
        while (at < end && fc_lookup(&j->f, at)) {
            at++;
        }
        for (n = 0; at + n < end && n < r.room && (n == 0 || !fc_lookup(&j->f, at + n)) &&
                    (r.recs[n] = fc_new()) != NULL;
             n++) {
            r.phys[n] = fc_phys(r.recs[n]);
        }
        spin_unlock_irqrestore(&fc_lock, flags);
        if (n == 0) {
            break;
        }
        got = fc_fill(j->ofd, at, n, r.phys);
        for (i = 0; i < n; i++) {
            if (got > (int32_t)i) {
                flags = spin_lock_irqsave(&fc_lock);
                if (j->mark == at + i) {
                    r.recs[i]->pfn |= FC_MARK;
                    j->mark = FC_NO_MARK;
                }
                spin_unlock_irqrestore(&fc_lock, flags);
            }
            if (got <= (int32_t)i || !fc_insert(&j->f, at + i, r.recs[i], j->epoch)) {
                fc_free(r.recs[i]);
            }
        }
        if (got < (int32_t)n) {
            break;
        }
        at += n;
    }
    fc_run_free(&r);
}

/** Let go of a job's mount and then of its description `ofd`, which still
 * holds the mount across the leave. The description's last unref reaches
 * `vfs_desc_close`, whose reap of a lazily unmounted mount skips one that an
 * operation is still in (`vfs_reap_detached`), and a leave reaps nothing, so
 * the leave goes first, as Linux's last `mntput` of a detached mount
 * schedules its cleanup from any context (fs/namespace.c:1426-1454). */
static void fc_job_drop(struct open_file *ofd)
{
    vfs_leave(ofd->vmnt);
    fd_ofd_unref(ofd);
}

/** A worker, kreadahead: take the oldest waiting job, read it, end it and
 * wake its waiters, and sleep while no job waits. It reads without the big
 * lock, as ext2 reads a file (`reads_unlocked`), and marks itself in a call
 * (`in_syscall`) only across a job, since a kernel thread so marked between
 * them would count as one that is exiting (`proc_is_kthread`). */
static void fc_worker(void)
{
    struct fc_job *j;
    struct open_file *ofd;
    uint32_t i, flags;

    bkl_exit();
    arch_irq_enable();
    for (;;) {
        sched_service_point();
        (void)wq_wait_event_idle(&work_wq, fc_job_waiting, NULL);
        j = NULL;
        flags = spin_lock_irqsave(&fc_lock);
        for (i = 0; i < FC_JOBS; i++) {
            if (jobs[i].ofd && !jobs[i].running &&
                (!j || jobs[i].serial - j->serial > 0x80000000u)) {
                j = &jobs[i];
            }
        }
        if (j) {
            j->running = 1;
            workers_busy++;
        }
        spin_unlock_irqrestore(&fc_lock, flags);
        if (!j) {
            continue;
        }
        /* The disk sleeps a caller in a system call and polls for any
         * other; the job and a last close sleep as a call does, and the
         * tick does not switch a kernel thread. */
        current_proc->in_syscall = 1;
        fc_job_read(j);
        flags = spin_lock_irqsave(&fc_lock);
        ofd = j->ofd;
        j->ofd = NULL;
        j->serial = 0;
        j->running = 0;
        workers_busy--;
        spin_unlock_irqrestore(&fc_lock, flags);
        wq_wake_all(&done_wq);
        fc_job_drop(ofd);
        current_proc->in_syscall = 0;
    }
}

/** Make one more worker, while there are fewer than FC_WORKERS, under the
 * big lock, which keeps the process table. Process context. */
static void fc_worker_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    bkl_enter();
    flags = spin_lock_irqsave(&fc_lock);
    if (workers >= FC_WORKERS) {
        spin_unlock_irqrestore(&fc_lock, flags);
        bkl_exit();
        return;
    }
    workers++;
    spin_unlock_irqrestore(&fc_lock, flags);
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        flags = spin_lock_irqsave(&fc_lock);
        workers--;
        spin_unlock_irqrestore(&fc_lock, flags);
        bkl_exit();
        klog(KLOG_WARN, "filecache", "readahead: worker: refused: no record or stack for it");
        return;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "kreadahead");
    setup_process_stack(p, fc_worker);
    sched_make_ready(p);
    kirq_restore(flags);
    bkl_exit();
    klog(KLOG_DEBUG, "filecache", "readahead: worker: kreadahead is pid %d", (int)p->pid);
}

/** End every job when there is no worker, which none could be made for,
 * since nothing else would end them and their readers wait. */
static void fc_cancel_orphans(void)
{
    struct open_file *ofds[FC_JOBS];
    uint32_t i, n = 0, flags = spin_lock_irqsave(&fc_lock);

    for (i = 0; workers == 0 && i < FC_JOBS; i++) {
        if (jobs[i].ofd) {
            ofds[n++] = jobs[i].ofd;
            jobs[i].ofd = NULL;
            jobs[i].serial = 0;
        }
    }
    spin_unlock_irqrestore(&fc_lock, flags);
    if (n > 0) {
        wq_wake_all(&done_wq);
    }
    for (i = 0; i < n; i++) {
        fc_job_drop(ofds[i]);
    }
}

/**
 * Queue the `count` pages of `f`, open as `ofd`, from `start` for a worker,
 * cut at the file's end, with `mark` the page to mark when it is in the
 * window; `epoch` is `fc_changes` taken before. Nothing is queued for a
 * window past the end, one whose first page a job will read already, one
 * on a mount being unmounted, from the boot before init has its pid, which
 * a worker would take, or when every slot is taken or no worker can be
 * made, since a reader that misses then reads the page itself. The caller
 * holds the description and no spinlock, and may sleep.
 */
static void fc_queue(struct open_file *ofd, const struct fc_file *f, uint32_t start, uint32_t count,
                     uint32_t mark, uint32_t epoch)
{
    uint64_t span = (f->size + PAGE_SIZE - 1u) / PAGE_SIZE;
    uint32_t i, flags, waiting = 0;
    int slot = -1, more;

    if (!buckets || count == 0 || start >= span || !current_proc || current_proc->pid == 0) {
        return;
    }
    if (count > span - start) {
        count = (uint32_t)(span - start);
    }
    if (mark - start >= count) {
        mark = FC_NO_MARK;
    }
    if (vfs_enter(ofd->vmnt) != 0) {
        return;
    }
    fd_ofd_ref(ofd);
    flags = spin_lock_irqsave(&fc_lock);
    if (fc_job_at(f, start) < 0) {
        for (i = 0; i < FC_JOBS; i++) {
            if (!jobs[i].ofd) {
                slot = slot < 0 ? (int)i : slot;
            } else if (!jobs[i].running) {
                waiting++;
            }
        }
    }
    if (slot >= 0) {
        jobs[slot].ofd = ofd;
        jobs[slot].f = *f;
        jobs[slot].start = start;
        jobs[slot].count = count;
        jobs[slot].mark = mark;
        jobs[slot].epoch = epoch;
        jobs[slot].running = 0;
        job_serials = job_serials % 0xFFFFFFFFu + 1u; /* never 0 */
        jobs[slot].serial = job_serials;
    }
    more = slot >= 0 && workers < FC_WORKERS && waiting + 1u > workers - workers_busy;
    spin_unlock_irqrestore(&fc_lock, flags);
    if (slot < 0) {
        fc_job_drop(ofd);
        return;
    }
    if (more) {
        fc_worker_start();
        fc_cancel_orphans();
    }
    wq_wake_all(&work_wq);
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
 * Queue the window after a marked page, `index`, that a read of `ofd`
 * wanting `req` pages from there reached: Linux's `page_cache_async_ra`
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
    fc_queue(ofd, f, ra->start, ra->size, ra->start, *fc_changes(f->mnt, f->ino));
}

/**
 * Read page `pgoff` of `f`, open as `ofd`, into the frame of `p`, which
 * `fc_new` gave, for a read that wants `req` pages from there, and with it
 * in the same call the pages after it of the window readahead gives
 * (`fc_sync_ra`) that are not present, up to FC_RUN_MOST in all, each into
 * a record and frame of its own, which then join the cache, as Linux's
 * `page_cache_ra_unbounded` reads its window into new folios
 * (mm/readahead.c). The call ends at the first page that is present or no
 * frame is had for, and the rest of the window is queued for a worker
 * first (`fc_queue`), so that its read runs beside this one. The page the
 * window marks takes the mark when it is read. `epoch` is the cache's count
 * taken before the read. The bytes of `pgoff` the file has after the read
 * (`fc_size_now`), which stop short of a page only at its end; 0 past the
 * end; or a negative errno.
 */
static int32_t fc_read_window(struct open_file *ofd, const struct fc_file *f, uint32_t pgoff,
                              uint32_t req, struct fc_page *p, uint32_t epoch)
{
    struct fc_run r;
    uint32_t n = 1, i, flags, mark, nr = fc_sync_ra(ofd, f, pgoff, req, &mark);
    uint64_t end = (uint64_t)pgoff * PAGE_SIZE, size;
    int32_t got;

    fc_run_init(&r, nr);
    r.recs[0] = p;
    r.phys[0] = fc_phys(p);
    flags = spin_lock_irqsave(&fc_lock);
    while (n < nr && n < r.room && (uint64_t)(pgoff + n) * PAGE_SIZE < f->size &&
           !fc_present(f, pgoff + n) && (r.recs[n] = fc_new()) != NULL) {
        r.phys[n] = fc_phys(r.recs[n]);
        n++;
    }
    spin_unlock_irqrestore(&fc_lock, flags);
    if (n < nr) {
        fc_queue(ofd, f, pgoff + n, nr - n, mark, epoch);
    }
    got = fc_fill(ofd, pgoff, n, r.phys);
    for (i = 1; i < n; i++) {
        if (got > (int32_t)i && pgoff + i == mark) {
            r.recs[i]->pfn |= FC_MARK;
        }
        if (got <= (int32_t)i || !fc_insert(f, pgoff + i, r.recs[i], epoch)) {
            fc_free(r.recs[i]);
        }
    }
    fc_run_free(&r);
    if (got <= 0) {
        return got;
    }
    size = fc_size_now(ofd, f->size);
    if (size <= end) {
        return 0;
    }
    return size - end < PAGE_SIZE ? (int32_t)(size - end) : (int32_t)PAGE_SIZE;
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

/**
 * Find page `pgoff` of `f`, open as `ofd`, for a read that wants pages up
 * to `last`: the frame of the page in the cache, with a reference for the
 * caller, after a wait for the job that reads the page when one will; or,
 * for a page to read, the frame of a new record, which `*p` receives; or 0
 * with no frame to be had. A marked page reached, in the cache or in a
 * job's window, queues the next window (`fc_async_ra`) before the page is
 * had or waited for, as Linux's `filemap_get_pages` starts it before it
 * waits on the folio's lock (mm/filemap.c:2584-2593).
 */
static uintptr_t fc_get(struct open_file *ofd, const struct fc_file *f, uint32_t pgoff,
                        uint32_t last, struct fc_page **p)
{
    uint32_t wait[2] = {0, 0}, flags;
    struct fc_page *hit;
    uintptr_t phys = 0;
    int marked, job;

    for (;;) {
        marked = 0;
        flags = spin_lock_irqsave(&fc_lock);
        hit = pages ? fc_lookup(f, pgoff) : NULL;
        job = hit ? -1 : fc_job_at(f, pgoff);
        if (hit) {
            phys = fc_phys(hit);
            frame_ref(phys);
            fc_use(hit);
            marked = (hit->pfn & FC_MARK) != 0;
            hit->pfn &= ~FC_MARK;
        } else if (job >= 0) {
            wait[0] = (uint32_t)job;
            wait[1] = jobs[job].serial;
            marked = jobs[job].mark == pgoff;
            if (marked) {
                jobs[job].mark = FC_NO_MARK;
            }
        } else {
            *p = fc_new();
            phys = *p ? fc_phys(*p) : 0;
        }
        spin_unlock_irqrestore(&fc_lock, flags);
        if (marked) {
            fc_async_ra(ofd, f, pgoff, last - pgoff + 1u);
        }
        if (job < 0) {
            return phys;
        }
        (void)wq_wait_event_nointr(&done_wq, fc_job_ended, wait);
    }
}

/**
 * Read up to `left` bytes at `at` of `f`, open as `ofd`, inside one page,
 * into the user buffer `dst`, for a read that wants pages up to `last`:
 * from the cache's frame, with a reference taken so that a drop meanwhile
 * does not free it (`fc_get`), or into a new frame read from the file with
 * the window that readahead gives (`fc_read_window`), which then joins the
 * cache. With no frame to be had the file system
 * reads the page's part into `dst` itself (`fc_read_direct`). The bytes
 * read, which stop short of the page's end or of `left` only at the end of
 * the file, as its size reads once the page is had (`fc_size_now`);
 * -EFAULT; or the file system's error.
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
        got = fc_read_window(ofd, f, pgoff, last - pgoff + 1u, p, epoch);
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
    fc_queue(ofd, &f, (uint32_t)first, want < max ? (uint32_t)want : max, FC_NO_MARK,
             *fc_changes(f.mnt, f.ino));
}

void filecache_dontneed(struct open_file *ofd, uint64_t offset, uint64_t len)
{
    struct fc_inode *file;
    struct fc_page *p, *next;
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
    for (p = file ? file->pages : NULL; p; p = next) {
        /* The last page frees the record, and has no sibling to go on to. */
        next = p->sibling;
        if (p->pgoff >= first && p->pgoff <= final) {
            fc_drop(p);
        }
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
    uint32_t flags;

    if (!fc_file_of(mnt, node, &f)) {
        return;
    }
    flags = spin_lock_irqsave(&fc_lock);
    (*fc_changes(f.mnt, f.ino))++;
    file = pages ? fc_find(f.mnt, f.ino) : NULL;
    if (file) {
        fc_drop_file(file);
    }
    spin_unlock_irqrestore(&fc_lock, flags);
}

void filecache_forget_ino(const struct vfs_mount *mnt, uint64_t ino)
{
    struct fc_inode *file;
    uint32_t flags;

    if (!buckets) {
        return;
    }
    mnt = vfs_mount_base(mnt);
    flags = spin_lock_irqsave(&fc_lock);
    (*fc_changes(mnt, ino))++;
    file = pages ? fc_find(mnt, ino) : NULL;
    if (file) {
        fc_drop_file(file);
    }
    spin_unlock_irqrestore(&fc_lock, flags);
}

void filecache_forget_mount(const struct vfs_mount *mnt)
{
    struct fc_inode *file, *next;
    uint32_t i, flags = spin_lock_irqsave(&fc_lock);

    mnt = vfs_mount_base(mnt);
    for (i = 0; i < FC_EPOCHS; i++) {
        changes[i]++;
    }
    for (i = 0; pages && i <= table_mask; i++) {
        for (file = files[i]; file; file = next) {
            next = file->next;
            if (file->mnt == mnt) {
                fc_drop_file(file);
            }
        }
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
