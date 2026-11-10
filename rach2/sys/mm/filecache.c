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

/* The most pages one miss of read(2) reads from the file at once. */
#define FC_RUN 16u

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

/** Free `p`, and its file's record when it was the file's last page. */
static void fc_drop(struct fc_page *p)
{
    fc_unlink(p);
    free_frame((uintptr_t)p->pfn * PAGE_SIZE);
    kfree(p);
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

/** Whether the cache has page `pgoff` of `f` as the file is now. */
static int fc_has(const struct fc_file *f, uint32_t pgoff)
{
    uint32_t flags = kirq_save();
    int has = fc_lookup(f, pgoff) != NULL;

    kirq_restore(flags);
    return has;
}

/** A page record with its frame for a new page: the least recently used
 * page's, off its lists, when the cache may take no more frames or heap,
 * else a new one; NULL when there is neither. Interrupts must be off. */
static struct fc_page *fc_take(void)
{
    struct fc_page *p;
    uintptr_t phys;

    if (pages >= most || frame_free_count() < frame_total_count() / 4u) {
        p = oldest;
        if (p) {
            fc_unlink(p);
            p->sibling = NULL;
            p->sibling_link = NULL;
            p->file = NULL;
        }
        return p;
    }
    p = kzalloc(sizeof(*p));
    if (!p) {
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

/** Keep a copy of page `pgoff` of `f`, from the frame `phys` or, when it is
 * 0, from `buf`, in place of any it had, unless a change of the file has
 * been recorded since `epoch`. A page read from the file as it was before is
 * dropped with the rest of the file's pages. */
static void fc_store(const struct fc_file *f, uint32_t pgoff, uintptr_t phys, const void *buf,
                     uint32_t epoch)
{
    struct fc_page *p, *old, **bucket;
    struct fc_inode *file, *spare = NULL;
    uint32_t flags;

    if (!buckets) {
        return;
    }
    flags = kirq_save();
    file = fc_find(f->mnt, f->ino);
    kirq_restore(flags);
    if (!file || !fc_current(file, f)) {
        spare = kzalloc(sizeof(*spare));
        if (!spare) {
            return;
        }
    }
    flags = kirq_save();
    if (epoch != *fc_changes(f->mnt, f->ino) || !(p = fc_take())) {
        kirq_restore(flags);
        kfree(spare);
        return;
    }
    if (!fc_copy((uintptr_t)p->pfn * PAGE_SIZE, NULL, phys, buf)) {
        kirq_restore(flags);
        free_frame((uintptr_t)p->pfn * PAGE_SIZE);
        kfree(p);
        kfree(spare);
        return;
    }
    p->pgoff = pgoff;
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
    if (!file) {
        if (!spare) {
            /* The file's record went since the look without the lock. */
            kirq_restore(flags);
            free_frame((uintptr_t)p->pfn * PAGE_SIZE);
            kfree(p);
            return;
        }
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

/** Store the pages of `run`, the `got` bytes read from page `pgoff` of `f`
 * on, each whole page and a last part that ends at the file's end, which is
 * kept with zeros after it; a part that ends short of the end is not kept. */
static void fc_store_run(const struct fc_file *f, uint32_t pgoff, uint8_t *run, uint32_t got,
                         uint32_t epoch)
{
    uint32_t i, have;

    for (i = 0; i * PAGE_SIZE < got; i++) {
        have = got - i * PAGE_SIZE;
        if (have < PAGE_SIZE) {
            if ((uint64_t)(pgoff + i) * PAGE_SIZE + have != f->size) {
                return;
            }
            k_memset(run + i * PAGE_SIZE + have, 0, PAGE_SIZE - have);
        }
        fc_store(f, pgoff + i, 0, run + i * PAGE_SIZE, epoch);
    }
}

int filecache_file_read(struct open_file *ofd, char __user *buf, uint32_t count, uint64_t offset,
                        int32_t *got)
{
    struct fc_file f;
    uint8_t *run;
    uint32_t done = 0;
    int32_t rc = 0;

    if (!buckets || count == 0 || !fc_file_of(ofd->vmnt, ofd->vnode, &f) ||
        (offset < f.size && (f.size - 1u) / PAGE_SIZE > 0xFFFFFFFFu)) {
        return 0;
    }
    if (offset >= f.size) {
        *got = 0;
        return 1;
    }
    if (count > f.size - offset) {
        count = (uint32_t)(f.size - offset);
    }
    run = kmalloc(FC_RUN * PAGE_SIZE);
    if (!run) {
        return 0;
    }
    while (done < count) {
        uint64_t at = offset + done;
        uint32_t pgoff = (uint32_t)(at / PAGE_SIZE);
        uint32_t in = (uint32_t)(at % PAGE_SIZE);
        uint32_t take = PAGE_SIZE - in, n = 1, epoch;
        int32_t read;

        if (take > count - done) {
            take = count - done;
        }
        if (pages && fc_read(&f, pgoff, 0, run)) {
            if (copy_to_user(buf + done, run + in, take) != 0) {
                rc = -EFAULT;
                break;
            }
            done += take;
            continue;
        }
        while (n < FC_RUN && (uint64_t)(pgoff + n) * PAGE_SIZE < offset + count &&
               !fc_has(&f, pgoff + n)) {
            n++;
        }
        epoch = *fc_changes(f.mnt, f.ino);
        read = vfs_read(ofd->vmnt, ofd->fhandle, run, n * PAGE_SIZE, (uint64_t)pgoff * PAGE_SIZE);
        if (read < 0) {
            rc = read;
            break;
        }
        fc_store_run(&f, pgoff, run, (uint32_t)read, epoch);
        if ((uint32_t)read <= in) {
            break;
        }
        take = (uint32_t)read - in;
        if (take > count - done) {
            take = count - done;
        }
        if (copy_to_user(buf + done, run + in, take) != 0) {
            rc = -EFAULT;
            break;
        }
        done += take;
        if ((uint32_t)read < n * PAGE_SIZE) {
            break;
        }
    }
    kfree(run);
    *got = done > 0 ? (int32_t)done : rc;
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
        fc_drop(oldest);
        given++;
    }
    kirq_restore(flags);
    return given;
}
