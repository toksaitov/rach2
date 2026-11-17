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
 * The readahead rows start a window of a file as a read past its mark
 * does (`fc_read_async`), with a file system whose `read_frames` keeps the
 * completion and a reference on each frame, as a request does, and ends
 * the reads when the row says: the window's pages are in the cache locked,
 * and neither read nor lent, until the completion unlocks them; a failed
 * read drops them; and a read whose pages a change of the file dropped
 * while it was in flight finds no page to unlock, and its frames come back
 * when its request lets them go.
 *
 * The completion rows free a window between two of a completion's wakes,
 * by the starter's put and by another completion, and check that the wakes
 * after it read nothing of it and wake the right queues; the insertion row
 * fails a run's second insertion, by another reader's store of that page
 * between two of `fc_lock_run`'s, and checks that each frame left over is
 * freed once.
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
#define FRAMES (2u * FC_DROP_BATCH + 8u)
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

/* The byte the readahead rows' pages are filled with. */
#define RA_FILL 0x66

/* The read the readahead rows' file system has started and not ended: its
   completion, first page, count and frames, each frame with a reference of
   the read's own, as a request holds one. */
static struct block_frames_done ra_done;
static uint32_t ra_count;
static uintptr_t ra_frames[8];
static int ra_started;

/**
 * Start the read of the readahead rows' frames: keep the completion and a
 * reference on each frame, as `block_read_frames_async` does, and answer
 * that every page is started. A read that waits is no row's.
 */
int32_t vfs_read_frames(struct vfs_mount *mnt, void *handle, uint32_t first, uint32_t count,
                        const uintptr_t *frames, const struct block_frames_done *done)
{
    uint32_t i;

    (void)mnt;
    (void)handle;
    (void)first;
    if (!done || ra_started || count > 8u) {
        check(0, "only a readahead row reads through the file system, one read at a time");
        return -EIO;
    }
    ra_done = *done;
    ra_count = count;
    for (i = 0; i < count; i++) {
        ra_frames[i] = frames[i];
        frame_ref(frames[i]);
    }
    ra_started = 1;
    return (int32_t)count;
}

/** End the started read with `status`, its frames filled with RA_FILL when
 * it is 0, as the device's answer does, and let go of its references. */
static void ra_end(int status)
{
    uint32_t i;

    if (!ra_started) {
        check(0, "a read was started");
        return;
    }
    for (i = 0; status == 0 && i < ra_count; i++) {
        __builtin_memset(kmap_temp(ra_frames[i]), RA_FILL, PAGE_SIZE);
    }
    ra_done.fn(ra_done.arg, ra_done.index, ra_frames, ra_count, status);
    for (i = 0; i < ra_count; i++) {
        free_frame(ra_frames[i]);
    }
    ra_started = 0;
}

/** A request's most sectors. */
uint32_t block_max_sectors(block_device_t *device)
{
    (void)device;
    return BLOCK_REQ_MAX_SECTORS;
}

/** No row waits for a locked page. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    check(0, "no row waits for a locked page");
    return 0;
}

/* The queues woken since a row cleared `woken_count`, in order, and what
   runs at the next wake, once, as another processor would run between two
   wakes of a completion's. */
#define WOKEN_MOST 8u
static wait_queue_t *woken[WOKEN_MOST];
static uint32_t woken_count;
static void (*wake_hook)(void);

/** Nobody sleeps to be woken: record the queue, then run and clear
 *  `wake_hook`. */
void wq_wake_all(wait_queue_t *wq)
{
    void (*fn)(void) = wake_hook;

    if (woken_count < WOKEN_MOST) {
        woken[woken_count] = wq;
    }
    woken_count++;
    if (fn) {
        wake_hook = 0;
        fn();
    }
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
static struct fake_file file_b = {8, 1, 2 * PAGE_SIZE, 100};
static struct fake_file file_c = {9, 1, 3 * PAGE_SIZE, 100};
static struct fake_file large_file = {10, 1, (FC_DROP_BATCH + 3u) * PAGE_SIZE, 100};
static void (*give_way_hook)(void);
static uint32_t give_way_below;

/** Run one concurrent operation once the drop has freed some pages. */
void sched_give_way(void)
{
    void (*fn)(void) = give_way_hook;

    check(!fc_lock.locked, "the cache lock is free when the walk gives way");
    if (fn && filecache_pages() < give_way_below) {
        give_way_hook = 0;
        fn();
    }
}

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

/** Whether page `pgoff` of `node` is in the cache locked for its read,
 * marked when `mark` is 1. */
static int locked_page(struct fake_file *node, uint32_t pgoff, int mark)
{
    struct fc_file f;
    struct fc_page *p;

    p = fc_file_of(&mount, node, &f) ? fc_lookup(&f, pgoff) : NULL;
    return p && (p->pfn & FC_LOCK) && ((p->pfn & FC_MARK) != 0) == mark;
}

/** Start a window of both pages of `file_b`, page 1 marked, as a read past
 * its mark does; 1 when its read was started. */
static int ra_start(void)
{
    static struct open_file ofd;
    struct fc_file f;

    ofd.vmnt = &mount;
    ofd.vnode = &file_b;
    ofd.fhandle = &file_b;
    if (!fc_file_of(&mount, &file_b, &f)) {
        check(0, "the readahead row's file is a cached file");
        return 0;
    }
    fc_read_async(&ofd, &f, 0, 2, 1, filecache_epoch(&mount, &file_b));
    return ra_started && ra_count == 2;
}

/**
 * A window's pages are in the cache locked, page 1 marked, and neither
 * read, lent nor started again while its read runs; its answer unlocks
 * them. A failed read drops its pages. A change of the file while its read
 * runs drops them, and the answer then finds no page and lets the frames
 * go.
 */
static void row_readahead_locks(void)
{
    uintptr_t lent[2] = {0, 0};
    uint32_t before = filecache_pages(), free_before;
    struct fc_file f;

    check(ra_start() && filecache_pages() == before + 2 && locked_page(&file_b, 0, 0) &&
              locked_page(&file_b, 1, 1),
          "a window's pages go into the cache locked, its mark among them");
    check(!reads_as(&file_b, 0, 0) && !reads_as(&file_b, 0, RA_FILL),
          "a locked page is not read from the cache");
    check(fc_file_of(&mount, &file_b, &f) && fc_lend(&f, 0, 2, lent) == 0 && lent[0] == 0,
          "nor lent to a mapping");
    ra_started = 0;
    fc_read_async(NULL, &f, 0, 2, FC_NO_MARK, filecache_epoch(&mount, &file_b));
    check(!ra_started, "and a window over them starts no read");
    ra_started = 1;
    ra_end(0);
    check(reads_as(&file_b, 0, RA_FILL) && reads_as(&file_b, 1, RA_FILL) &&
              !locked_page(&file_b, 0, 0) && !locked_page(&file_b, 1, 0),
          "the read's answer unlocks the pages, which then read back");

    filecache_forget(&mount, &file_b);
    check(ra_start(), "a window is started again");
    ra_end(-EIO);
    check(filecache_pages() == before && !reads_as(&file_b, 0, RA_FILL),
          "a failed read drops its pages");

    free_before = (uint32_t)frame_free_count();
    check(ra_start(), "and again");
    filecache_forget(&mount, &file_b);
    check(filecache_pages() == before && frame_free_count() == free_before - 2u,
          "a change of the file drops the locked pages, whose frames the read still holds");
    ra_end(0);
    check(filecache_pages() == before && frame_free_count() == free_before,
          "and the read's answer finds no page and lets the frames go");
}

/* The window and frames of the completion rows, which their hooks end. */
static struct fc_flight *race_w;
static struct fc_run race_run;

/** The starter's put on another processor, as `fc_read_async` makes it
 *  once its read is submitted: its frames' references and its hold of the
 *  window, which is the last and frees the window. */
static void starter_put(void)
{
    free_frame(race_run.phys[0]);
    free_frame(race_run.phys[1]);
    fc_flight_put(race_w);
}

/** Another request's completion on another processor, of the window's
 *  page 2, which ends its last page and frees the window. */
static void other_done(void)
{
    fc_done(race_w, 2, race_run.phys + 2, 1, 0);
}

/**
 * Lock pages 0 to `n - 1` of `file_c` for a read as `fc_read_async` does,
 * filled with `fill` as the device's answer leaves them and held by the
 * request too, and give them a window whose `pending` is `pending`; 1 when
 * all `n` were put in.
 */
static int race_lock(uint32_t n, int fill, uint32_t pending)
{
    struct fc_file f;
    uint32_t i;

    if (!fc_file_of(&mount, &file_c, &f)) {
        check(0, "the completion rows' file is a cached file");
        return 0;
    }
    fc_run_init(&race_run, n);
    if (fc_lock_run(&f, 0, n, FC_NO_MARK, filecache_epoch(&mount, &file_c), &race_run) != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        frame_ref(race_run.phys[i]); /* the request's */
        __builtin_memset(kmap_temp(race_run.phys[i]), fill, PAGE_SIZE);
    }
    race_w = kmalloc(sizeof(*race_w));
    race_w->mnt = f.mnt;
    race_w->ino = f.ino;
    race_w->start = 0;
    race_w->pending = pending;
    return 1;
}

/** Whether the completion rows woke page `pgoff`'s queue `at`th. */
static int woke(uint32_t at, uint32_t pgoff)
{
    return at < woken_count && woken[at] == fc_page_wq(&mount, file_c.ino, pgoff);
}

/**
 * A completion's wakes against the free of its window. `fc_done` gives up
 * its pages and then wakes each page's queue: the starter's put, when only
 * its hold is left, or the completion of the window's last other pages, may
 * free the window on another processor between two of those wakes, and the
 * wakes that follow must not read it, which AddressSanitizer would stop,
 * and must still wake the pages' own queues. Each hook here frees the
 * window at the completion's first wake.
 */
static void row_done_free(void)
{
    uint32_t free_before = (uint32_t)frame_free_count(), i;

    check(race_lock(2, 0x5C, 3), "a window of two pages is locked, its starter still holding it");
    woken_count = 0;
    wake_hook = starter_put;
    fc_done(race_w, 0, race_run.phys, 2, 0);
    check(!wake_hook && woken_count == 2 && woke(0, 0) && woke(1, 1),
          "the starter's put frees the window at the first wake, and both pages' queues are woken");
    check(reads_as(&file_c, 0, 0x5C) && reads_as(&file_c, 1, 0x5C) && !locked_page(&file_c, 0, 0) &&
              !locked_page(&file_c, 1, 0),
          "the answer unlocks both pages, which read back");
    for (i = 0; i < 2; i++) {
        free_frame(race_run.phys[i]); /* the request lets go */
    }
    fc_run_free(&race_run);
    check(frame_free_count() == free_before - 2u, "the cache alone holds the two frames");
    filecache_forget(&mount, &file_c);

    check(race_lock(3, 0x5D, 3), "a window of three pages is locked, its starter gone");
    for (i = 0; i < 3; i++) {
        free_frame(race_run.phys[i]); /* the starter's, given back after its put */
    }
    woken_count = 0;
    wake_hook = other_done;
    fc_done(race_w, 0, race_run.phys, 2, 0);
    check(!wake_hook && woken_count == 3 && woke(0, 0) && woke(1, 2) && woke(2, 1),
          "a completion of the last page frees the window at another's first wake, and every "
          "page's queue is woken");
    check(reads_as(&file_c, 0, 0x5D) && reads_as(&file_c, 1, 0x5D) && reads_as(&file_c, 2, 0x5D),
          "both answers unlock their pages");
    for (i = 0; i < 3; i++) {
        free_frame(race_run.phys[i]);
    }
    fc_run_free(&race_run);
    filecache_forget(&mount, &file_c);
    check(frame_free_count() == free_before, "the forgotten pages give their frames back");
}

/** A concurrent reader's store of page 1 of `file_c`, between the insertions
 *  of a run's pages 0 and 1. */
static void store_page_1(void)
{
    check(insert_page(&file_c, 1, 0x77, 0), "the other reader's page 1 is kept");
}

/**
 * An insertion failure in the middle of a run. `fc_lock_run` makes a frame
 * for each of pages 0 to 2 and inserts them in turn; another reader's store
 * of page 1 after page 0's insertion fails page 1's, and the run is the one
 * page. Every frame left over holds the cache's reference and the
 * starter's, both of which the cleanup drops: a frame with only the first
 * would be freed by the first drop and lose another owner's reference at
 * the second.
 */
static void row_lock_run_fail(void)
{
    uint32_t free_before = (uint32_t)frame_free_count(), epoch;
    struct fc_file f;
    struct fc_run r;
    uint32_t n;

    if (!fc_file_of(&mount, &file_c, &f)) {
        check(0, "the insertion row's file is a cached file");
        return;
    }
    fc_run_init(&r, 3);
    epoch = filecache_epoch(&mount, &file_c);
    restore_hook = store_page_1;
    restore_countdown = 3; /* fc_lock_run's give, then page 0's insertion's two */
    n = fc_lock_run(&f, 0, 3, FC_NO_MARK, epoch, &r);
    check(!restore_hook && n == 1, "the run ends at the page the other reader stored");
    check(locked_page(&file_c, 0, 0) && reads_as(&file_c, 1, 0x77),
          "page 0 is in the cache locked, and page 1 is the other reader's");
    check(n == 1 && frame_refs[frame_index(r.phys[0])] == 2 &&
              frame_refs[frame_index(r.phys[1])] == 0 && frame_refs[frame_index(r.phys[2])] == 0,
          "page 0's frame keeps both references, and the frames left over are freed once each");
    free_frame(r.phys[0]); /* the starter's */
    fc_run_free(&r);
    filecache_forget(&mount, &file_c);
    check(frame_free_count() == free_before, "every frame of the run comes back");
}

/** Cache enough pages of one file to cross a batch boundary. */
static void fill_large_file(void)
{
    uint32_t i;

    for (i = 0; i < FC_DROP_BATCH + 3u; i++) {
        check(insert_page(&large_file, i, 0x11, 0), "the large file's page is kept");
    }
}

/** Reclaim an old page and replace it while its detached inode is drained. */
static void replace_invalidated_page(void)
{
    check(filecache_pages() == 3, "the invalidation gives way after one batch");
    check(!reads_as(&large_file, 0, 0x11), "the remaining stale pages are hidden");
    check(filecache_shrink(1) == 1, "reclaim can take a detached page");
    check(insert_page(&large_file, 0, 0x22, 0), "a new record accepts a replacement page");
}

/** Try an insertion with the current epoch while unmount is draining. */
static void store_during_unmount(void)
{
    check(!insert_page(&large_file, 0, 0x33, 0), "the unmount refuses even a fresh insertion");
}

/** A detached file allows a new record, while an unmount bars insertion. */
static void row_batched_drop(void)
{
    static struct vfs_mount other;
    static uint8_t buf[PAGE_SIZE];

    fill_large_file();
    give_way_below = filecache_pages();
    give_way_hook = replace_invalidated_page;
    filecache_forget(&mount, &large_file);
    check(!give_way_hook && filecache_pages() == 1 && reads_as(&large_file, 0, 0x22),
          "the invalidation drains the old record and leaves the new one");
    filecache_forget(&mount, &large_file);

    other.type = &vfs_ext2_type;
    other.state = VFS_ACTIVE;
    filecache_store_node(&other, &large_file, 0, buf, filecache_epoch(&other, &large_file));
    check(insert_page(&file_a, 0, 0x44, 0), "another file of the same mount is kept");
    fill_large_file();
    mount.state = VFS_UNMOUNTING;
    give_way_below = filecache_pages();
    give_way_hook = store_during_unmount;
    filecache_forget_mount(&mount);
    check(!give_way_hook && filecache_pages() == 1 && frame_free_count() == FRAMES - 1u &&
              filecache_read_node(&other, &large_file, 0, buf),
          "the unmount gives way, drops all its pages and leaves the other mount's page");
    filecache_forget(&other, &large_file);
    check(filecache_pages() == 0 && frame_free_count() == FRAMES,
          "every frame comes back after the other mount's page is dropped");
    give_way_hook = 0;
}

int main(void)
{
    mount.type = &vfs_ext2_type;
    mount.state = VFS_ACTIVE;
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

    row_readahead_locks();
    row_done_free();
    row_lock_run_fail();

    mount.state = VFS_UNMOUNTING;
    filecache_forget_mount(&mount);
    check(filecache_pages() == 0 && frame_free_count() == FRAMES,
          "every frame goes back with the mount's pages");
    mount.state = VFS_ACTIVE;
    row_batched_drop();
    return fails != 0;
}
