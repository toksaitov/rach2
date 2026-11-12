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
 * The readahead row queues a window of a file as a read past its mark does
 * and runs the job in `fc_worker`, made as `fc_worker_start` makes it,
 * until it sleeps for the next. The mount counts the operations in it, as
 * `vfs_enter` and `vfs_leave` do, and the description its references; the
 * description's last unref stands for `vfs_desc_close`, which reaps a
 * lazily unmounted mount only when no operation is in it, as
 * `vfs_reap_detached` does, while a leave reaps nothing. The volume is
 * lazily unmounted and its file's last descriptor closed while the job
 * reads, and the job's end must reap the mount.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <setjmp.h>

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

/* The readahead row's mount and description: the operations in the mount,
   whether it is lazily unmounted and whether it was reaped, and the
   description's references. Set while the row runs, which the stubs below
   allow alone. */
static int ra_row;
static uint32_t ra_inprogress, ra_refs;
static int ra_detached, ra_reaped;
static struct open_file ra_ofd;

/* The byte the readahead row's pages are filled with. */
#define RA_FILL 0x66

/**
 * Fill the frames of the readahead row's job with RA_FILL. The first call
 * also unmounts the volume lazily and closes the file's last descriptor,
 * the row's, so that only the job holds the description as it ends.
 */
int32_t vfs_read_frames(struct vfs_mount *mnt, void *handle, uint32_t first, uint32_t count,
                        const uintptr_t *frames)
{
    uint32_t i;

    (void)handle;
    (void)first;
    if (!ra_row || mnt != ra_ofd.vmnt) {
        check(0, "only the readahead row reads through the file system");
        return -EIO;
    }
    if (!ra_detached) {
        ra_detached = 1;
        fd_ofd_unref(&ra_ofd);
    }
    for (i = 0; i < count; i++) {
        __builtin_memset(kmap_temp(frames[i]), RA_FILL, PAGE_SIZE);
    }
    return (int32_t)count;
}

/** A request's most sectors. */
uint32_t block_max_sectors(block_device_t *device)
{
    (void)device;
    return BLOCK_REQ_MAX_SECTORS;
}

/** Enter the readahead row's mount, unless it was reaped, as a mount no
 *  longer active refuses. */
int vfs_enter(struct vfs_mount *mnt)
{
    if (!ra_row || mnt != ra_ofd.vmnt) {
        check(0, "only the readahead row enters a mount");
        return -EBUSY;
    }
    if (ra_reaped) {
        return -EBUSY;
    }
    ra_inprogress++;
    return 0;
}

/** Leave the readahead row's mount, which reaps nothing. */
void vfs_leave(struct vfs_mount *mnt)
{
    check(ra_row && mnt == ra_ofd.vmnt && ra_inprogress > 0,
          "a leave is of the readahead row's mount, which it entered");
    if (ra_inprogress > 0) {
        ra_inprogress--;
    }
}

/** Hold the readahead row's description. */
void fd_ofd_ref(struct open_file *ofd)
{
    check(ra_row && ofd == &ra_ofd, "only the readahead row holds a description");
    ra_refs++;
}

/** Let the readahead row's description go; the last unref is
 *  `vfs_desc_close`, whose put reaps a lazily unmounted mount that no
 *  operation is in. */
void fd_ofd_unref(struct open_file *ofd)
{
    check(ra_row && ofd == &ra_ofd && ra_refs > 0,
          "an unref is of the readahead row's description, which is held");
    if (ra_refs > 0 && --ra_refs == 0 && ra_detached && ra_inprogress == 0) {
        ra_reaped = 1;
    }
}

/** No row waits for a job. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    check(0, "no row waits for a readahead");
    return 0;
}

/* Where the readahead row's worker goes once no job waits. */
static jmp_buf worker_stop;

/** A worker's sleep for a job: none while one waits, and once none does,
 *  back to the row, which takes the worker's next sleep as its end. */
int wq_wait_event_idle(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    if (!ra_row) {
        check(0, "a worker runs only in the readahead row");
        return 0;
    }
    if (!cond(arg)) {
        longjmp(worker_stop, 1);
    }
    return 0;
}

/** Nobody sleeps to be woken. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** No interrupt comes. */
void arch_irq_enable(void)
{}

/** Nothing else runs. */
void sched_service_point(void)
{}

/* The readahead row's worker, the address its stack is given at, and the
   entry `setup_process_stack` gave it. */
static process_t worker_proc;
#define WORKER_STACK 0x10000u
static void (*worker_entry)(void);

/** A stack for the readahead row's worker; its address is never read. */
uintptr_t kstack_alloc(void)
{
    check(ra_row, "a worker is made only in the readahead row");
    return ra_row ? WORKER_STACK : 0;
}

/** No stack is given back. */
void kstack_free(uintptr_t top)
{
    (void)top;
    check(0, "no worker's stack is given back");
}

/** The readahead row's worker's record. */
process_t *alloc_process(void)
{
    return ra_row ? &worker_proc : NULL;
}

/** The worker's name is not read. */
void proc_set_comm(process_t *p, const char *path)
{
    (void)p;
    (void)path;
}

/** Note the worker's entry, which the row calls itself. */
void setup_process_stack(process_t *proc, void (*entry_point)(void))
{
    check(proc == &worker_proc, "the worker's record is the one made");
    worker_entry = entry_point;
}

/** The row runs the worker itself. */
void sched_make_ready(process_t *p)
{
    (void)p;
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

/**
 * Queue both pages of `file_b` as a read past its mark does, unmount its
 * volume lazily and close its last descriptor while the worker reads them,
 * and check that the job's end lets go of the mount and the description
 * and reaps the mount, as Linux's last `mntput` of a detached mount does.
 */
static void row_readahead_reaps(void)
{
    static process_t reader;
    struct fc_file f;

    ra_row = 1;
    ra_ofd.vmnt = &mount;
    ra_ofd.vnode = &file_b;
    ra_ofd.fhandle = &file_b;
    ra_refs = 1;
    reader.pid = 2;
    current_proc = &reader;
    if (!fc_file_of(&mount, &file_b, &f)) {
        check(0, "the readahead row's file is a cached file");
        return;
    }
    fc_queue(&ra_ofd, &f, 0, 2, FC_NO_MARK, filecache_epoch(&mount, &file_b));
    check(ra_inprogress == 1 && ra_refs == 2 && worker_entry == fc_worker,
          "a queued window enters its mount, holds its description and makes a worker");

    current_proc = &worker_proc;
    if (setjmp(worker_stop) == 0) {
        worker_entry();
    }
    current_proc = NULL;
    check(ra_detached && reads_as(&file_b, 0, RA_FILL) && reads_as(&file_b, 1, RA_FILL),
          "the worker reads the window while the volume is unmounted lazily");
    check(ra_refs == 0 && ra_inprogress == 0,
          "the job's end leaves the mount and lets the description go");
    check(ra_reaped, "and the description's last close reaps the lazily unmounted mount");
    ra_row = 0;
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

    row_readahead_reaps();

    filecache_forget_mount(&mount);
    check(filecache_pages() == 0 && frame_free_count() == FRAMES,
          "every frame goes back with the mount's pages");
    return fails != 0;
}
