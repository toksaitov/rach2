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
 * not kept, since the read may have seen part of it. A mapping gets a copy of
 * a cached page, so nothing it does reaches the cache. Each file that has
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
#include "../lib/kirq.h"
#include "../klog.h"

/* The fewest lists that each table has. */
#define FC_MIN_LISTS 1024u

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
    uint32_t pfn; /* the frame's number, its address over PAGE_SIZE */
};

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

/** Whether the pages of `file` were read from `f` as it is now. */
static int fc_current(const struct fc_inode *file, const struct fc_file *f)
{
    return file->generation == f->generation && file->size == f->size && file->mtime == f->mtime;
}

/** Page `pgoff` of `f` as the file is now, or NULL. Interrupts must be off. */
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
    left = frame_unref((uintptr_t)p->pfn * PAGE_SIZE);
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
    uint32_t flags = kirq_save();
    int hit = 0;

    p = fc_lookup(f, pgoff);
    if (p) {
        hit = fc_copy(phys, buf, (uintptr_t)p->pfn * PAGE_SIZE, NULL);
        if (hit) {
            fc_use(p);
        }
    }
    kirq_restore(flags);
    return hit;
}

/** A page record off every list, with a frame of its own, for a new page:
 * a new record and frame while a quarter of the frames are free and the
 * records fit in an eighth of the heap; past either, the least recently used
 * page's record, whose frame goes back, while a sixteenth of the frames are
 * free, above the reserve that `mem_may_take` keeps, so that the new frame
 * is had; NULL otherwise. Interrupts must be off. */
static struct fc_page *fc_new(void)
{
    struct fc_page *p;
    uintptr_t phys;
    size_t free_frames = frame_free_count(), total = frame_total_count();

    if (pages < most && free_frames >= total / 4u) {
        p = kzalloc(sizeof(*p));
        if (!p) {
            return NULL;
        }
    } else if (oldest && free_frames >= total / 16u) {
        p = oldest;
        fc_unlink(p);
        /* A reader of the page holds a reference of its own. */
        free_frame((uintptr_t)p->pfn * PAGE_SIZE);
        k_memset(p, 0, sizeof(*p));
    } else {
        return NULL;
    }
    phys = alloc_user_frame();
    if (phys == 0) {
        kfree(p);
        return NULL;
    }
    p->pfn = (uint32_t)(phys / PAGE_SIZE);
    return p;
}

/** Free `p`, which `fc_new` gave and is on no list, and its frame. */
static void fc_free(struct fc_page *p)
{
    free_frame((uintptr_t)p->pfn * PAGE_SIZE);
    kfree(p);
}

/** Put `p`, which `fc_new` gave and whose frame holds page `pgoff` of `f`
 * as just read, into the cache in place of any page it had there, unless a
 * change of the file has been recorded since `epoch`; a page read from the
 * file as it was before is dropped with the rest of the file's pages. 1, or
 * 0 when `p` stays the caller's. */
static int fc_insert(const struct fc_file *f, uint32_t pgoff, struct fc_page *p, uint32_t epoch)
{
    struct fc_page *old, **bucket;
    struct fc_inode *file, *spare = NULL;
    uint32_t flags = kirq_save();

    file = fc_find(f->mnt, f->ino);
    kirq_restore(flags);
    if (!file || !fc_current(file, f)) {
        spare = kzalloc(sizeof(*spare));
        if (!spare) {
            return 0;
        }
    }
    flags = kirq_save();
    if (epoch != *fc_changes(f->mnt, f->ino)) {
        kirq_restore(flags);
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
            fc_drop(old);
            file = fc_find(f->mnt, f->ino);
            break;
        }
    }
    if (!file && !spare) {
        /* The file's record went since the look without the lock. */
        kirq_restore(flags);
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
    kirq_restore(flags);
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
    flags = kirq_save();
    p = fc_new();
    kirq_restore(flags);
    if (!p) {
        return;
    }
    copied = fc_copy((uintptr_t)p->pfn * PAGE_SIZE, NULL, phys, buf);
    if (!copied || !fc_insert(f, pgoff, p, epoch)) {
        fc_free(p);
    }
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

/**
 * Read up to `left` bytes at `at` of `f`, open as `ofd`, inside one page,
 * into the user buffer `dst`: from the cache's frame, with a reference taken
 * so that a drop meanwhile does not free it, or into a new frame read from
 * the file, which then joins the cache when it holds a whole page or the
 * last part of the file. With no frame to be had the file system reads the
 * whole `left` into `dst` itself. The bytes read, which stop short of the
 * page's end or of `left` only at the end of the file; -EFAULT; or the file
 * system's error.
 */
static int32_t fc_read_page(struct open_file *ofd, const struct fc_file *f, uint64_t at,
                            char __user *dst, uint32_t left)
{
    uint32_t pgoff = (uint32_t)(at / PAGE_SIZE), in = (uint32_t)(at % PAGE_SIZE);
    uint32_t take = PAGE_SIZE - in < left ? PAGE_SIZE - in : left, epoch = 0, flags;
    struct fc_page *p = NULL, *hit;
    uintptr_t phys = 0;
    int32_t got = PAGE_SIZE, rc = 0;
    uint8_t *page;

    flags = kirq_save();
    hit = pages ? fc_lookup(f, pgoff) : NULL;
    if (hit) {
        phys = (uintptr_t)hit->pfn * PAGE_SIZE;
        frame_ref(phys);
        fc_use(hit);
    } else {
        p = fc_new();
        phys = p ? (uintptr_t)p->pfn * PAGE_SIZE : 0;
    }
    kirq_restore(flags);
    page = phys ? kmap_temp(phys) : NULL;
    if (!page) {
        if (p) {
            fc_free(p);
        } else if (phys) {
            free_frame(phys);
        }
        return vfs_read(ofd->vmnt, ofd->fhandle, (void __force *)dst, left, at);
    }
    if (p) {
        epoch = *fc_changes(f->mnt, f->ino);
        got = vfs_read(ofd->vmnt, ofd->fhandle, page, PAGE_SIZE, (uint64_t)pgoff * PAGE_SIZE);
        if (got >= 0 && got < (int32_t)PAGE_SIZE) {
            k_memset(page + got, 0, PAGE_SIZE - (uint32_t)got);
        }
        if (got < 0) {
            rc = got;
        } else if ((uint32_t)got <= in) {
            take = 0;
        } else if (take > (uint32_t)got - in) {
            take = (uint32_t)got - in;
        }
    }
    if (rc == 0 && take > 0 && copy_to_user(dst, page + in, take) != 0) {
        rc = -EFAULT;
    }
    kunmap_temp(page);
    if (!p) {
        free_frame(phys);
    } else if (got <= 0 ||
               (got < (int32_t)PAGE_SIZE &&
                (uint64_t)pgoff * PAGE_SIZE + (uint32_t)got != f->size) ||
               !fc_insert(f, pgoff, p, epoch)) {
        fc_free(p);
    }
    return rc != 0 ? rc : (int32_t)take;
}

int filecache_file_read(struct open_file *ofd, char __user *buf, uint32_t count, uint64_t offset,
                        int32_t *got)
{
    struct fc_file f;
    uint32_t done = 0;
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
    while (done < count) {
        uint32_t in = (uint32_t)((offset + done) % PAGE_SIZE);
        uint32_t want = PAGE_SIZE - in < count - done ? PAGE_SIZE - in : count - done;

        n = fc_read_page(ofd, &f, offset + done, buf + done, count - done);
        if (n <= 0) {
            break;
        }
        done += (uint32_t)n;
        if ((uint32_t)n < want) {
            break;
        }
    }
    *got = done > 0 || n > 0 ? (int32_t)done : n;
    return 1;
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
    flags = kirq_save();
    (*fc_changes(f.mnt, f.ino))++;
    file = pages ? fc_find(f.mnt, f.ino) : NULL;
    if (file) {
        fc_drop_file(file);
    }
    kirq_restore(flags);
}

void filecache_forget_mount(const struct vfs_mount *mnt)
{
    struct fc_inode *file, *next;
    uint32_t i, flags = kirq_save();

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
    kirq_restore(flags);
}

uint32_t filecache_pages(void)
{
    return pages;
}

uint32_t filecache_shrink(uint32_t want)
{
    uint32_t given = 0, flags = kirq_save();

    while (oldest && given < want) {
        given += (uint32_t)fc_drop(oldest);
    }
    kirq_restore(flags);
    return given;
}
