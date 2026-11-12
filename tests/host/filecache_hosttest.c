/**
 * Host test of the file page cache, mm/filecache.c: the insertion of a page
 * read while another processor frees the file's record.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/bkl_none.c
 *
 * filecache.c is included whole, so that a row calls `fc_new` and
 * `fc_insert` as `fc_store` does. The frames are a table of host pages with
 * a count of references each, and the file is a record whose identity,
 * size and modification time `vfs_getattr` gives. The interrupt flag's
 * restore, which follows every give of `fc_lock`, runs a hook a row sets on
 * the restore it names, which stands for another processor that takes the
 * lock the moment it is free.
 *
 * The rows: a page stored is read back; a second copy of a page held is
 * not kept, and the page held stays, as Linux's -EEXIST; a page inserted
 * while a shrink between the insertion's two holds of `fc_lock` frees the
 * file's record, the record of its one page, is not kept, and nothing reads
 * the freed record, which AddressSanitizer would stop; a page whose file
 * changed, so that its insertion makes a new record, is kept under that
 * record though the old one is freed between the holds; and every frame
 * goes back.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "mm/filecache.c"

extern int printf(const char *format, ...);

static int fails;

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("filecache-hosttest: fail: %s\n", row);
        fails++;
    }
}

struct percpu cpu_table[NR_CPUS];

/* The frames: page `i` is the frame at `(i + 1) * PAGE_SIZE`, free while
   its count is 0. */
#define FRAMES 64u
static uint8_t frame_mem[FRAMES][PAGE_SIZE];
static uint32_t frame_refs[FRAMES];

/** The index in the table of the frame at `phys`. */
static uint32_t frame_index(uintptr_t phys)
{
    uint32_t i = (uint32_t)(phys / PAGE_SIZE) - 1u;

    check(phys % PAGE_SIZE == 0 && i < FRAMES, "a frame is one of the table's");
    return i < FRAMES ? i : 0;
}

/** A free frame with one reference, or 0 when every frame is taken. */
uintptr_t alloc_user_frame(void)
{
    uint32_t i;

    for (i = 0; i < FRAMES; i++) {
        if (frame_refs[i] == 0) {
            frame_refs[i] = 1;
            return (uintptr_t)(i + 1u) * PAGE_SIZE;
        }
    }
    return 0;
}

/** Take a reference to the frame at `phys`. */
void frame_ref(uintptr_t phys)
{
    frame_refs[frame_index(phys)]++;
}

/** Drop a reference to the frame at `phys`; the references left. */
uint32_t frame_unref(uintptr_t phys)
{
    uint32_t i = frame_index(phys);

    check(frame_refs[i] > 0, "a frame let go has a reference");
    if (frame_refs[i] > 0) {
        frame_refs[i]--;
    }
    return frame_refs[i];
}

/** Drop a reference to the frame at `phys`. */
void free_frame(uintptr_t phys)
{
    (void)frame_unref(phys);
}

/** The frames of the table. */
size_t frame_total_count(void)
{
    return FRAMES;
}

/** The frames nobody holds. */
size_t frame_free_count(void)
{
    size_t n = 0;
    uint32_t i;

    for (i = 0; i < FRAMES; i++) {
        n += frame_refs[i] == 0;
    }
    return n;
}

/** The host page of the frame at `phys`. */
void *kmap_temp(uintptr_t phys)
{
    return frame_mem[frame_index(phys)];
}

/** Nothing to unmap. */
void kunmap_temp(void *va)
{
    (void)va;
}

/** Copy the frame at `src` to the frame at `dst`; 0. */
int paging_copy_frame(uintptr_t dst, uintptr_t src)
{
    __builtin_memcpy(kmap_temp(dst), kmap_temp(src), PAGE_SIZE);
    return 0;
}

/** No row copies to a program. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    (void)dst;
    (void)src;
    (void)n;
    check(0, "no row copies to a program");
    return -EFAULT;
}

/** The kernel log; the records are dropped. */
void klog(int level, const char *sys, const char *fmt, ...)
{
    (void)level;
    (void)sys;
    (void)fmt;
}

/* The type `fc_file_of` takes for ext2's; nothing of it is read. */
const struct vfs_type vfs_ext2_type;

/* A file: what `vfs_getattr` gives for the node that points here. */
struct fake_file {
    uint64_t ino;
    uint32_t generation;
    uint64_t size;
    int64_t mtime;
};

/** A mount is its own base. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m)
{
    return m;
}

/** The attributes of the regular file `node`, a `struct fake_file`. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    const struct fake_file *file = node;

    (void)mnt;
    __builtin_memset(st, 0, sizeof(*st));
    st->mode = 0100644u;
    st->ino = file->ino;
    st->generation = file->generation;
    st->size = file->size;
    st->mtime_sec = file->mtime;
    return 0;
}

/** No row reads through the file system. */
int32_t vfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count, uint64_t offset)
{
    (void)mnt;
    (void)handle;
    (void)buf;
    (void)count;
    (void)offset;
    check(0, "no row reads through the file system");
    return -EIO;
}

/** No row reads through the file system's frames either. */
int32_t vfs_read_frames(struct vfs_mount *mnt, void *handle, uint32_t first, uint32_t count,
                        const uintptr_t *frames)
{
    (void)mnt;
    (void)handle;
    (void)first;
    (void)count;
    (void)frames;
    check(0, "no row reads through the file system");
    return -EIO;
}

/* Run once at the restore `restore_countdown` counts down to, then
   cleared. */
static void (*restore_hook)(void);
static int restore_countdown;

/** No interrupt can come, so there is no flag to save. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Run and clear `restore_hook` at the restore a row named. */
void kirq_restore(uint32_t flags)
{
    void (*fn)(void) = restore_hook;

    (void)flags;
    if (fn && --restore_countdown == 0) {
        restore_hook = 0;
        fn();
    }
}

/* The frames the hook's shrink gave back. */
static uint32_t shrunk;

/** Shrink the whole cache, as a reclaim on another processor would: the
 *  file's last page goes and its record is freed with it. */
static void shrink_all(void)
{
    shrunk = filecache_shrink(FRAMES);
}

static struct vfs_mount mount;
static struct fake_file file_a = {7, 1, 3 * PAGE_SIZE, 100};

/**
 * Insert page `pgoff` of `node`, filled with `fill`, as `fc_store` does,
 * with `hook` run at the first give of `fc_lock` inside `fc_insert`, the
 * one between its two holds; 1 when the page was kept.
 */
static int insert_page(struct fake_file *node, uint32_t pgoff, int fill, void (*hook)(void))
{
    struct fc_file f;
    struct fc_page *p;
    uint32_t flags, epoch = filecache_epoch(&mount, node);
    int kept;

    if (!fc_file_of(&mount, node, &f)) {
        check(0, "the fake file is a cached file");
        return 0;
    }
    flags = spin_lock_irqsave(&fc_lock);
    p = fc_new();
    spin_unlock_irqrestore(&fc_lock, flags);
    if (!p) {
        check(0, "a page record and frame are had");
        return 0;
    }
    __builtin_memset(kmap_temp((uintptr_t)p->pfn * PAGE_SIZE), fill, PAGE_SIZE);
    restore_hook = hook;
    restore_countdown = 1;
    kept = fc_insert(&f, pgoff, p, epoch);
    restore_hook = 0;
    if (!kept) {
        fc_free(p);
    }
    return kept;
}

/** Whether page `pgoff` of `node` reads back as bytes of `fill`. */
static int reads_as(struct fake_file *node, uint32_t pgoff, int fill)
{
    static uint8_t out[PAGE_SIZE];
    uint32_t i;

    if (!filecache_read_node(&mount, node, pgoff, out)) {
        return 0;
    }
    for (i = 0; i < PAGE_SIZE; i++) {
        if (out[i] != (uint8_t)fill) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    mount.type = &vfs_ext2_type;
    init_filecache();
    check(buckets != NULL, "the tables are made");

    check(insert_page(&file_a, 0, 0x11, 0) && filecache_pages() == 1,
          "a page is kept under a new record");
    check(reads_as(&file_a, 0, 0x11), "and reads back");
    check(!insert_page(&file_a, 0, 0x55, 0) && filecache_pages() == 1,
          "a second copy of a page held is not kept");
    check(reads_as(&file_a, 0, 0x11), "and the page held stays");

    shrunk = 0;
    check(!insert_page(&file_a, 1, 0x22, shrink_all),
          "a page whose current record is freed between the holds is not kept");
    check(shrunk == 1 && filecache_pages() == 0,
          "the shrink between the holds took the record's one page");
    check(!reads_as(&file_a, 1, 0x22), "and the page is not found");

    check(insert_page(&file_a, 0, 0x33, 0) && filecache_pages() == 1,
          "a page is kept under a new record again");
    file_a.mtime = 101;
    shrunk = 0;
    check(insert_page(&file_a, 1, 0x44, shrink_all),
          "a page of a changed file is kept though its old record goes between the holds");
    check(shrunk == 1 && filecache_pages() == 1, "under the record the insertion made");
    check(reads_as(&file_a, 1, 0x44) && !reads_as(&file_a, 0, 0x33),
          "which has the new page and not the old");

    filecache_forget_mount(&mount);
    check(filecache_pages() == 0 && frame_free_count() == FRAMES,
          "every frame goes back with the mount's pages");
    return fails != 0;
}
