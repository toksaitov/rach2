/*
 * filemap.c: the files mmap maps, as filemap.h describes them.
 */

#include "filemap.h"

#include "kheap.h"
#include "mm.h"
#include "notify.h"
#include "paging.h"
#include "shmem.h"
#include "usercopy.h"
#include "../irq.h"
#include "../fs/krffs.h"
#include "../fs/tmpfs.h"
#include "../fs/vfs.h"
#include "../block/block.h"
#include "../block/loop.h"
#include "../klog.h"
#include "../proc/process.h"
#include "../proc/smp.h"
#include "../proc/wait_queue.h"
#include "../timer.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../lib/kuser.h"
#include "../lib/spinlock.h"
#include "filecache.h"
#include "../network/socket/socket.h"
#include "../syscall_internal.h"

/*
 * A disk file's resident pages belong to its private and shared mappings.
 * A shared store reaches every clean mapping; a private store copies first.
 * A page is read on first touch and written back whole, up to the file's
 * size, which a mapping does not change. A shared writer writes back only
 * the pages a store has reached (`stored`), which a shared mapping maps
 * read-only until its first write fault (`mkwrite`). The mount and inode
 * number identify the file to descriptor operations.
 *
 * The table of `sh` changes only under the file's lock (`vfs_node_lock`):
 * a page joins it at a fault, and a truncation or the last release takes
 * pages out. Its arrays, the directory and the leaves, are made, replaced
 * and freed only under the big lock as well, so a reader under the big lock
 * alone walks them safely and sees each entry either empty or holding a
 * page read from the file; a fault puts a page in a leaf that exists under
 * the file's lock alone (`shmem_has_leaf`), as Linux adds a folio to a
 * file's pages under the mapping's own xarray lock with no global one
 * (__filemap_add_folio, mm/filemap.c:842-950), and takes the big lock only
 * to make the leaf. A fault looks a page up under the file's lock, as
 * Linux's filemap_fault finds the folio and locks it (mm/filemap.c:3326-3370).
 *
 * A failed write of a record's pages is recorded in its file system's
 * write-back errors (`vfs_wb_err_set`) and nowhere else, so that each
 * description's cursor reports it once, at its fsync, fdatasync, syncfs or
 * msync with MS_SYNC, as Linux's mapping_set_error records it and
 * file_check_and_advance_wb_err reports it (mm/filemap.c:726). The record
 * keeps no error of its own and goes with its last mapping.
 */
struct dshare {
    struct dshare *next;
    const struct vfs_mount *mnt;
    uint64_t ino;
    uint32_t users; /* the mappings of it */
    int stores;     /* a shared mapping through a descriptor open for
                       writing has mapped it, so its pages may hold stores
                       the file lacks; cleared when its last mapping goes */
    /* The faults that read the file less the faults that found their page
     * in memory and the pages a fault mapped from memory before any read,
     * as Linux keeps a file's mmap_miss (mm/filemap.c:3187-3189,
     * 3712-3716, and do_async_mmap_readahead), at most
     * `DISK_MISSES_MOST`; read-around stops above `DISK_LOTS_MISSED`.
     * Under the file's lock. */
    uint32_t misses;
    /* The tick before which every store of its mappings has reached the
     * file: its first mapping's, then the start of each write-back of every
     * page that succeeded. What a write-back puts in the file may have been
     * dirty since then, which it tells the flusher (`vfs_writeback_aged`),
     * since the pages keep no age of their own. */
    uint64_t clean;
    wait_queue_t left;
    struct shmem sh;
    /* The pages a shared mapping may have stored into, a bit each, in
     * leaves of SHMEM_LEAF bits made at a page's first write fault
     * (`filemap_mkwrite_disk`), as Linux marks a folio dirty at
     * page_mkwrite: only these are written back, so that a page read from a
     * hole and never stored to leaves the hole a hole. A bit stays once
     * set, and a truncation clears the bits of the pages it frees. Under
     * the file's lock. */
    uint32_t **stored;
    uint32_t nstored; /* the leaves `stored` reaches */
};

/* The words of a leaf of `stored`. */
#define DSHARE_STORED_WORDS (SHMEM_LEAF / 32u)

/* The record: the region map's part first, so the one is found from the
 * other, and the copy of the descriptor; for a shared mapping, the pages it
 * maps, and for shared anonymous memory those pages are its own; for a
 * shared mapping of a disk file, the file's pages in memory. */
struct filemap {
    struct vm_file vf;
    fd_entry_t entry;
    struct filemap *next;
    struct shmem *sh;
    struct shmem own;
    struct dshare *ds;
    int shared;   /* stores may change the file, and may be written back */
    int writing;  /* its release is writing shared pages back */
    int retained; /* 1: own hold after a failed release; 2: retry succeeded */
    /* Shared anonymous memory's inode number on the mount
     * `vfs_memfd_mount` has; 0 for any other. */
    uint64_t anon_ino;
};

static struct filemap *maps;
/* The disk files shared mappings hold pages of, in buckets by inode
 * number, since every read and write of a disk file asks whether its file
 * is among them: Linux reaches a file's pages from its inode
 * (`inode->i_mapping`) without a search. A record joins and leaves its
 * bucket, and its `stores` is set, under both the big lock and
 * `dshare_lock`, so a holder of either may walk the buckets; a read without
 * the big lock asks under the spinlock (`filemap_disk_stores`), under which
 * nothing else is taken. */
#define DSHARE_BUCKETS 256u
static struct dshare *dshares[DSHARE_BUCKETS];
static uint32_t dshare_count; /* how many are in the buckets */
/* How many in each bucket have `stores` set, read as one word first, so
 * that a read of a file in a bucket with none takes no lock. */
static uint32_t dshare_stores_in[DSHARE_BUCKETS];
static struct spinlock dshare_lock = SPINLOCK_INIT_NAMED("dshare");

/**
 * Whether a page of a file on `mnt` can be read with interrupts off, for a
 * thread inside a call that cannot sleep. A file in memory or on the KRFFS
 * root can, and so can one on a disk the block layer polls there. A loop
 * device, which reads through the file system that holds its file and sleeps
 * its caller, and a USB stick, which answers only through its interrupts,
 * cannot.
 */
static int filemap_fault_readable(const struct vfs_mount *mnt)
{
    uint32_t flags;

    if (!mnt || !mnt->dev) {
        return 1;
    }
    flags = block_flags(mnt->dev);
    return (flags & BLOCK_FAULT_SAFE) || ((flags & BLOCK_POLLABLE) && !(flags & BLOCK_CALLER_SYNC));
}

/**
 * Read page `pgoff` of the file `m` maps into `page`: the count read, or a
 * negative errno. A file on a device is read as a system call reads it,
 * since a polled read would stop the whole kernel for as long as the disk
 * takes. A thread outside every call, faulting from user mode or writing a
 * signal's frame on its way back there, is marked in a call and runs with
 * interrupts on for the read, as `terminate_current_process` runs a
 * process's end, so the device may sleep it and the timer does not switch
 * away in the middle; the fault holds the file across the read and looks its
 * region up again after (`file_fault`). A thread inside a call with
 * interrupts off cannot sleep: it reads a device in place where
 * `filemap_fault_readable` allows, and its read fails with EIO elsewhere.
 */
static int32_t filemap_read(struct filemap *m, uint8_t *page, uint32_t pgoff)
{
    const struct vfs_mount *mnt = m->entry.ofd ? m->entry.ofd->vmnt : NULL;
    uint32_t flags = 0;
    int entered = 0;
    int32_t got;

    if (!(kirq_enabled() && proc_in_syscall())) {
        if (mnt && mnt->dev && current_proc && !current_proc->in_syscall) {
            current_proc->in_syscall = 1;
            flags = kirq_save();
            arch_irq_enable();
            entered = 1;
        } else if (!filemap_fault_readable(mnt)) {
            return -EIO;
        }
    }
    got = fs_read_at(&m->entry, page, PAGE_SIZE, (uint64_t)pgoff * PAGE_SIZE);
    if (entered) {
        kirq_restore(flags);
        current_proc->in_syscall = 0;
    }
    return got;
}

/** A page of a private mapping: the copy the file cache has of it, 0, or
 * the page read from the file (`filemap_read`), which the cache then keeps
 * a copy of, 1; -1 when the read fails. */
static int filemap_fill(struct vm_file *f, uint32_t pgoff, uintptr_t phys)
{
    struct filemap *m = (struct filemap *)f;
    uint32_t epoch = m->entry.ofd ? filecache_epoch(m->entry.ofd->vmnt, m->entry.ofd->vnode) : 0;
    uint8_t *page;
    int32_t got;

    if (filecache_read(&m->entry, pgoff, phys)) {
        return 0;
    }
    page = kmap_temp(phys);
    if (!page) {
        return -1;
    }
    got = filemap_read(m, page, pgoff);
    if (got >= 0 && got < (int32_t)PAGE_SIZE) {
        k_memset(page + got, 0, PAGE_SIZE - (uint32_t)got);
    }
    kunmap_temp(page);
    if (got < 0) {
        return -1;
    }
    filecache_store(&m->entry, pgoff, phys, epoch);
    return 1;
}

/** A page of a shared mapping of memory: shared anonymous memory has every
 * page, and a file in memory those its size reaches, as on Linux, where a
 * touch past a file's end, or one its quota has no room for, is SIGBUS.
 * Returns 0 with a reference in `phys`, ENXIO past EOF or with the quota
 * full, or ENOMEM when no frame can be had. */
static int filemap_share(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    struct filemap *m = (struct filemap *)f;

    if (m->sh != &m->own && (uint64_t)pgoff << 12 >= m->sh->size) {
        return -ENXIO;
    }
    *phys = shmem_map_page(m->sh, pgoff);
    if (*phys) {
        return 0;
    }
    return shmem_quota_full(m->sh) ? -ENXIO : -ENOMEM;
}

/** The KRFFS boot-root node the mapping's file is now, including through a
 * bind mount, or NULL for a file of another type. */
static void *filemap_node(const struct filemap *m)
{
    return fd_root_node(&m->entry);
}

/** The index in `dshares` of the bucket of inode number `ino`. */
static uint32_t dshare_index(uint64_t ino)
{
    return (uint32_t)(ino ^ (ino >> 32)) % DSHARE_BUCKETS;
}

/** The bucket of `dshares` that holds the file of inode number `ino`. */
static struct dshare **dshare_bucket(uint64_t ino)
{
    return &dshares[dshare_index(ino)];
}

/** The pages in memory of the file `ino` of `mnt`, or NULL when no shared
 * mapping has them. */
static struct dshare *dshare_of(const struct vfs_mount *mnt, uint64_t ino)
{
    struct dshare *ds;

    if (dshare_count == 0) {
        return NULL;
    }
    mnt = vfs_mount_base(mnt);
    for (ds = *dshare_bucket(ino); ds && !(ds->mnt == mnt && ds->ino == ino); ds = ds->next) {
    }
    return ds;
}

/** Take `ds`, which is in its bucket, out of it, under the big lock;
 * nothing here sleeps. */
static void dshare_unlink(struct dshare *ds)
{
    struct dshare **link = dshare_bucket(ds->ino);
    uint32_t flags = spin_lock_irqsave(&dshare_lock);

    while (*link && *link != ds) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = ds->next;
        dshare_count--;
        if (ds->stores) {
            dshare_stores_in[dshare_index(ds->ino)]--;
        }
    }
    spin_unlock_irqrestore(&dshare_lock, flags);
}

/** Copy a mapping's descriptor of `ds` into `out` with a reference of its
 * own, through a writable shared mapping when `writer`. Returns 1 when
 * found, 0 otherwise. Under the big lock; neither the copy nor the reference
 * sleeps. Zero-reference and releasing mappings still own their descriptor
 * until they leave `maps`, so later last puts cannot hide their stores or
 * make sync follow new releases to obtain a writer. */
static int dshare_backing(const struct dshare *ds, int writer, struct filemap *out)
{
    const struct filemap *m;

    for (m = maps; m; m = m->next) {
        if (m->ds == ds && (!writer || (m->shared && (m->entry.flags & FD_FLAG_WRITE)))) {
            out->entry = m->entry;
            out->shared = m->shared;
            fd_ofd_ref(out->entry.ofd);
            return 1;
        }
    }
    return 0;
}

/** Whether page `i` of `ds` may hold a store of a shared mapping
 * (`stored`). Under the file's lock. */
static int dshare_stored(const struct dshare *ds, uint32_t i)
{
    uint32_t leaf = i / SHMEM_LEAF, bit = i % SHMEM_LEAF;

    return leaf < ds->nstored && ds->stored[leaf] &&
           (ds->stored[leaf][bit / 32u] >> (bit % 32u) & 1u);
}

/** Mark page `i` of `ds` as one a shared mapping may store into, making
 * the leaf and growing the directory as needed. Under the file's lock.
 * 0, or -ENOMEM with nothing changed. */
static int dshare_mark(struct dshare *ds, uint32_t i)
{
    uint32_t leaf = i / SHMEM_LEAF, bit = i % SHMEM_LEAF, n;
    uint32_t **dir;

    if (leaf >= ds->nstored) {
        n = leaf + 1u > ds->nstored * 2u ? leaf + 1u : ds->nstored * 2u;
        dir = kzalloc((size_t)n * sizeof(*dir));
        if (!dir) {
            return -ENOMEM;
        }
        if (ds->stored) {
            k_memcpy(dir, ds->stored, ds->nstored * sizeof(*dir));
            kfree(ds->stored);
        }
        ds->stored = dir;
        ds->nstored = n;
    }
    if (!ds->stored[leaf]) {
        ds->stored[leaf] = kzalloc(DSHARE_STORED_WORDS * sizeof(uint32_t));
        if (!ds->stored[leaf]) {
            return -ENOMEM;
        }
    }
    ds->stored[leaf][bit / 32u] |= 1u << (bit % 32u);
    return 0;
}

/** Clear the marks of `ds` (`stored`) from page `first` on, the pages a
 * truncation freed, so that a page read again after the file grows back is
 * clean. Under the file's lock. */
static void dshare_unmark_from(struct dshare *ds, uint32_t first)
{
    uint32_t leaf, w;

    for (leaf = first / SHMEM_LEAF; leaf < ds->nstored; leaf++) {
        if (!ds->stored[leaf]) {
            continue;
        }
        if (leaf > first / SHMEM_LEAF || first % SHMEM_LEAF == 0) {
            kfree(ds->stored[leaf]);
            ds->stored[leaf] = NULL;
            continue;
        }
        w = (first % SHMEM_LEAF) / 32u;
        if (first % 32u) {
            ds->stored[leaf][w] &= (1u << (first % 32u)) - 1u;
            w++;
        }
        for (; w < DSHARE_STORED_WORDS; w++) {
            ds->stored[leaf][w] = 0;
        }
    }
}

/** Let go of a hold on `ds`, a mapping's or a write-back's, and with the
 * last take the record out of its bucket and free it, its pages and its
 * marks. Nothing here sleeps. */
static void dshare_put(struct dshare *ds)
{
    if (--ds->users != 0) {
        return;
    }
    dshare_unlink(ds);
    shmem_free(&ds->sh);
    dshare_unmark_from(ds, 0);
    kfree(ds->stored);
    kfree(ds);
}

/** Record `rc`, a failed write of pages of a file through `m`, in the
 * write-back errors of the file's file system (`vfs_wb_err_set`), its one
 * record, which each description's cursor reports once, as Linux's
 * `mapping_set_error` records a failed write-back of a file's folios
 * (include/linux/pagemap.h:43). */
static void dshare_failed(struct filemap *m, int rc)
{
    vfs_wb_err_set(m->entry.ofd->vmnt, rc);
}

/**
 * Write `len` bytes of frame `phys` at `at` through `m`, under the file's
 * lock. A small stack buffer keeps no temporary mapping across a sleep
 * and needs no allocation at unmap or exit. 0 or the first write error.
 */
static int dshare_write_bytes(struct filemap *m, uintptr_t phys, uint32_t len, uint64_t at)
{
    uint8_t buf[1024];
    uint32_t done = 0;

    while (done < len) {
        uint32_t take = len - done < sizeof(buf) ? len - done : (uint32_t)sizeof(buf);
        uint8_t *page = kmap_temp(phys);
        int32_t n;

        if (!page) {
            return -EIO;
        }
        k_memcpy(buf, page + done, take);
        kunmap_temp(page);
        n = fs_write_at(&m->entry, buf, take, at + done);
        if (n != (int32_t)take) {
            return n < 0 ? n : -EIO;
        }
        done += take;
    }
    return 0;
}

/**
 * Write page `i` of `ds` back through `m`'s copy of the descriptor, by way
 * of a stack buffer (`dshare_write_bytes`). The file's lock
 * (`vfs_node_lock`: the mount's namespace lock, or the file's own on a type
 * that sets `file_locked`) is held from before the file's size is read to
 * the end of the write, so a truncation, which takes it too, runs wholly
 * before or after: the write never reaches past the size it leaves, and the
 * page, looked up after the size is read and copied with nothing between that
 * sleeps, is one a truncation has not freed. A page no store has reached
 * (`dshare_stored`) is not written, as Linux's write-back passes a clean
 * folio by (`write_cache_pages`, mm/page-writeback.c): it equals the file,
 * and writing a page read from a hole would allocate the hole's blocks. A
 * partial page's tail is zeroed before writing, as Linux's mpage_writepage
 * zeros bytes beyond EOF. A file a loop device stands on is not written, as
 * write(2) does not write it. 1 after a write, 0 when there was nothing to
 * write, -EBUSY, or the write's error.
 */
static int dshare_write_page(struct dshare *ds, struct filemap *m, uint32_t i)
{
    struct vfs_mount *mnt = m->entry.ofd->vmnt;
    uint64_t at = (uint64_t)i * PAGE_SIZE;
    struct vfs_ref file;
    struct vfs_stat st;
    uintptr_t phys;
    uint8_t *page;
    uint32_t len;
    int rc = 0;

    file.mnt = mnt;
    file.node = m->entry.ofd->vnode;
    vfs_node_lock(&file);
    if (!dshare_stored(ds, i)) {
        rc = 0;
    } else if (loop_backs(mnt, m->entry.ofd->vnode)) {
        rc = -EBUSY;
    } else if (vfs_getattr(mnt, m->entry.ofd->vnode, &st) != 0) {
        rc = -EIO;
    } else if (at < st.size && (phys = shmem_resident(&ds->sh, i)) != 0) {
        page = kmap_temp(phys);
        if (!page) {
            rc = -EIO;
        } else {
            len = st.size - at < PAGE_SIZE ? (uint32_t)(st.size - at) : PAGE_SIZE;
            k_memset(page + len, 0, PAGE_SIZE - len);
            kunmap_temp(page);
            rc = dshare_write_bytes(m, phys, len, at);
            rc = rc == 0 ? 1 : rc;
        }
    }
    vfs_node_unlock(&file);
    return rc;
}

/** Write the pages of `ds` from page `first` up to page `end` back through
 * `m`'s copy of the descriptor (`dshare_write_page`). What was written may
 * have been dirty since `clean`, which the file's mount keeps for the
 * flusher (`vfs_writeback_aged`), and a write of every page that succeeds
 * moves `clean` to its own start. Returns 0, also when `m` has no
 * description, or the first failure. No allocation is needed. */
static int dshare_write_back(struct dshare *ds, struct filemap *m, uint32_t first, uint32_t end)
{
    uint64_t start = tick_count();
    uint32_t i;
    int rc = 0, wrote = 0;

    if (!m || !m->entry.ofd || !m->entry.ofd->vmnt) {
        return 0;
    }
    for (i = first; i < end && i < ds->sh.npages; i++) {
        int page_rc = shmem_resident(&ds->sh, i) ? dshare_write_page(ds, m, i) : 0;

        if (page_rc > 0) {
            wrote = 1;
        } else if (page_rc != 0 && rc == 0) {
            rc = page_rc;
        }
    }
    if (wrote) {
        vfs_writeback_aged(m->entry.ofd->vmnt, ds->clean);
    }
    if (rc == 0 && first == 0 && end >= ds->sh.npages) {
        ds->clean = start;
    }
    return rc;
}

/* A sync waits only for releases published at or before its boundary. */
struct dshare_drain {
    const struct dshare *ds;
    uint64_t gen;
};

/** Whether every release of `arg`'s file published before its boundary
 * has finished writing. Under the big lock; later releases do not delay
 * this wait. */
static int dshare_settled(void *arg)
{
    const struct dshare_drain *drain = arg;
    const struct filemap *m;

    for (m = maps; m; m = m->next) {
        if (m->ds == drain->ds && m->writing && m->vf.dead_gen <= drain->gen) {
            return 0;
        }
    }
    return 1;
}

/** Drop the own holds of failed releases after a successful write-back of
 * every page of `ds`, whatever error is kept for fsync, so that their pages,
 * marks and descriptors go; the releases wait for `vm_file_reap`. Whether
 * any hold was dropped. */
static int dshare_retried(struct dshare *ds)
{
    struct filemap *m;
    int dropped = 0;

    for (m = maps; m; m = m->next) {
        if (m->ds == ds && m->retained == 1) {
            m->retained = 2;
            vm_file_put(&m->vf);
            dropped = 1;
        }
    }
    return dropped;
}

/**
 * Write the pages of `ds` from `first` up to `end` back through a shared
 * mapping's description open for writing, and with `own`, the mapping an
 * msync was given, and `flush`, for MS_SYNC, run the file system's fsync
 * after them through `own`'s description, whose cursor into the write-back
 * errors moves, as Linux's msync syncs the region's own file
 * (mm/msync.c:87). Take a zero-publication boundary and own references to
 * both descriptions before sleeping: only releases at or before that
 * boundary are drained or waited for. The backing descriptor also writes
 * stores of a mapping whose last put follows the boundary, including one
 * between the reap and the write, without waiting for its release. The page
 * range is bounded before the drain, so later mappings and faults cannot
 * extend the call.
 *
 * A failed page write is recorded in the file system's write-back errors
 * (`dshare_failed`), which every description's cursor reports once; with
 * `own` it is the answer too, with or without `flush`, and `own`'s cursor
 * moves past it, as Linux's fsync reports a failed write-back once to the
 * file that ran it, so that the description's next fsync does not report it
 * again. A full write whose pages all went back drops failed releases' holds
 * (`dshare_retried`) for a later reap, whatever the fsync after it answers.
 * Under the big lock. Returns 0 or the first failure.
 */
static int dshare_sync(struct dshare *ds, uint32_t first, uint32_t end, struct filemap *own,
                       int flush)
{
    struct filemap backing;
    struct dshare_drain drain;
    struct open_file *mine = NULL;
    int have, rc, wrote = 0;

    ds->users++;
    drain.ds = ds;
    drain.gen = vm_file_reap_start();
    end = end < ds->sh.npages ? end : ds->sh.npages;
    k_memset(&backing, 0, sizeof(backing));
    have = dshare_backing(ds, 1, &backing);
    if (own && own->entry.ofd && own->entry.ofd->vmnt) {
        mine = own->entry.ofd;
        fd_ofd_ref(mine);
    }
    vm_file_reap_until(drain.gen);
    (void)wq_wait_event_nointr(&ds->left, dshare_settled, &drain);
    if (have) {
        wrote = dshare_write_back(ds, &backing, first, end);
        if (wrote != 0) {
            dshare_failed(&backing, wrote);
        }
    }
    rc = wrote;
    if (mine && wrote != 0) {
        (void)vfs_fsync_error(mine->vmnt, &mine->wb_seen); /* past `wrote` */
    } else if (mine && flush) {
        rc = vfs_fsync(mine->vmnt, mine->fhandle, &mine->wb_seen);
    }
    if (wrote == 0 && first == 0 && end >= ds->sh.npages) {
        (void)dshare_retried(ds);
    }
    dshare_put(ds);
    if (have) {
        fd_ofd_unref(backing.entry.ofd);
    }
    if (mine) {
        fd_ofd_unref(mine);
    }
    return rc;
}

/** Add the frame `phys`, with the reference it was read or lent with, to
 * the pages in memory of the disk file of `m` as page `pgoff`, under the
 * file's lock, which the caller holds, and under the big lock as well when
 * the page's leaf of the table must be made, since readers under the big
 * lock alone walk the table's arrays (struct dshare). Returns the frame
 * with a mapping reference, or 0 when no leaf could be had. */
static uintptr_t disk_adopt(struct filemap *m, uint32_t pgoff, uintptr_t phys)
{
    int big = !bkl_held() && !shmem_has_leaf(m->sh, pgoff);

    if (big) {
        bkl_enter();
    }
    phys = shmem_adopt(m->sh, pgoff, phys);
    if (big) {
        bkl_exit();
    }
    return phys;
}

/* Linux's mmap read-around: the pages a fault that reads the file reads,
 * its own among them, from half of them before it, the default readahead
 * window of 128 KiB (VM_READAHEAD_PAGES, include/linux/pagemap.h:1375),
 * while the file's misses stay at most MMAP_LOTSAMISS
 * (do_sync_mmap_readahead, mm/filemap.c:3092, 3187-3203). */
#define DISK_AROUND      32u
#define DISK_LOTS_MISSED 100u
#define DISK_MISSES_MOST (DISK_LOTS_MISSED * 10u)

/** Read page `pgoff` of the disk file of `m` into the frame `phys`,
 * clearing what lies past the file's end: the bytes read, or a negative
 * errno. */
static int32_t disk_read_page(struct filemap *m, uint32_t pgoff, uintptr_t phys)
{
    uint8_t *page = kmap_temp(phys);
    int32_t got;

    if (!page) {
        return -EIO;
    }
    got = filemap_read(m, page, pgoff);
    if (got >= 0 && got < (int32_t)PAGE_SIZE) {
        k_memset(page + got, 0, PAGE_SIZE - (uint32_t)got);
    }
    kunmap_temp(page);
    return got;
}

/**
 * Read pages `first` to `first + count - 1` of the disk file of `m` into
 * the frames `frames`, each filled whole: in one call of the type's
 * `read_frames`, which fills them straight from the device, or a page at
 * a time (`disk_read_page`) for a type without it. The pages filled, from
 * the first, or a negative errno.
 */
static int32_t disk_read_pages(struct filemap *m, uint32_t first, uint32_t count,
                               const uintptr_t *frames)
{
    int32_t got =
        vfs_read_frames(m->entry.ofd->vmnt, m->entry.ofd->fhandle, first, count, frames, NULL);
    uint32_t i;

    if (got != -ENOSYS) {
        return got;
    }
    for (i = 0; i < count; i++) {
        got = disk_read_page(m, first + i, frames[i]);
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
 * Read page `pgoff` of the disk file of `m`, which neither its pages in
 * memory nor the file page cache has, into the frame `phys`, and with it the
 * pages of the read-around window that neither has, as Linux reads the
 * window in one request (do_sync_mmap_readahead, mm/filemap.c:3197-3203):
 * each run of such pages in one call (`disk_read_pages`), into frames the
 * device fills itself. The other pages join the file page cache alone, its
 * frames shared as `filecache_keep` allows, for the fault around this one
 * and the next faults to take, and what the cache declines is let go. Only
 * a file whose frames may be the cache's (`stores` clear), and one the
 * cache keeps (`filecache_keeps`), reads around. The caller holds the
 * file's lock and may sleep; `size` is the file's size and `epoch` the
 * cache's count taken before the read.
 *
 * When the window's read leaves `pgoff` unfilled, the page is read again
 * into a new frame, which replaces `*phys`, and the caller's reference on
 * the old one is let go: a read the block layer gave up on may still be
 * the device's to fill (`block_read_frames`), and its request keeps a
 * reference on the frame until the device answers or is reset, as Linux
 * leaves a folio locked and not up to date until its bio ends (fs/mpage.c,
 * `filemap_read_folio` in mm/filemap.c). Returns the bytes of `pgoff` read,
 * with `*phys` the caller's still on an error, or a negative errno.
 */
static int32_t disk_read_around(struct filemap *m, uint32_t pgoff, uint64_t size, uintptr_t *phys,
                                uint32_t epoch)
{
    uintptr_t around[DISK_AROUND];
    uint32_t start = pgoff > DISK_AROUND / 2u ? pgoff - DISK_AROUND / 2u : 0;
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t n = pages - start < DISK_AROUND ? (uint32_t)(pages - start) : DISK_AROUND;
    uint32_t i, run, own = pgoff - start, wanted = 0, filled = 0;
    uintptr_t fresh;
    int32_t got = -EIO;

    for (i = 0; i < n; i++) {
        around[i] = i == own ? *phys : shmem_resident(m->sh, start + i);
        wanted |= around[i] ? 0 : 1u << i;
    }
    (void)filecache_lend(&m->entry, start, n, around);
    wanted |= 1u << own;
    for (i = 0; i < n; i++) {
        if (i == own || !(wanted & (1u << i))) {
            continue;
        }
        if (around[i]) {
            free_frame(around[i]); /* the reference the cache lent it with */
            wanted &= ~(1u << i);
        } else if ((around[i] = alloc_user_frame()) == 0) {
            wanted &= ~(1u << i);
        }
    }
    for (i = 0; i < n; i += run) {
        if (!(wanted & (1u << i))) {
            run = 1;
            continue;
        }
        for (run = 1; i + run < n && (wanted & (1u << (i + run))); run++) {
        }
        got = disk_read_pages(m, start + i, run, around + i);
        if (got > 0) {
            filled |= (got >= 32 ? 0xFFFFFFFFu : (1u << got) - 1u) << i;
        }
    }
    for (i = 0; i < n; i++) {
        if (i == own || !(wanted & (1u << i))) {
            continue;
        }
        if (filled & (1u << i)) {
            filecache_keep(&m->entry, start + i, around[i], epoch);
        }
        free_frame(around[i]);
    }
    if (!(filled & (1u << own))) {
        if ((fresh = alloc_user_frame()) == 0) {
            return -ENOMEM;
        }
        got = disk_read_page(m, pgoff, fresh);
        if (got < 0) {
            free_frame(fresh);
            return got;
        }
        free_frame(*phys);
        *phys = fresh;
        return got;
    }
    return size - (uint64_t)pgoff * PAGE_SIZE < PAGE_SIZE
               ? (int32_t)(size - (uint64_t)pgoff * PAGE_SIZE)
               : (int32_t)PAGE_SIZE;
}

/**
 * Read or borrow disk page `pgoff` under its file's lock, which excludes a
 * write or truncation until the page joins the file's resident pages. A
 * caller without the big lock, a fault of a file whose reads run without it
 * (`unlocked`), finds a resident page and adds one it read under the file's
 * lock alone, and takes the big lock only when the page's leaf of the table
 * must be made (`disk_adopt`). A page not resident comes from the file page
 * cache when it has the page, and a page read from the file is kept there,
 * so the pages outlive the file's last mapping, as Linux's page cache
 * outlives the mappings of its folios: a program started again or a library
 * mapped again does not come from the disk again. While no shared mapping
 * through a writable descriptor has mapped the file (`stores`), the page in
 * memory is the cache's frame itself, as Linux maps the page cache's folio
 * (`filemap_fault`, mm/filemap.c:3326-3370); after, it is a copy, since
 * such a mapping stores into it before the file. Returns 0 with a resident
 * mapping reference in `phys`, 1 after reading the file, or a negative errno.
 */
static int filemap_disk_page(struct filemap *m, uint32_t pgoff, uintptr_t *phys)
{
    struct vfs_ref file;
    struct vfs_stat st;
    uint32_t epoch = 0;
    int32_t got = PAGE_SIZE;
    int rc = 0, cached, share;

    file.mnt = m->entry.ofd->vmnt;
    file.node = m->entry.ofd->vnode;
    vfs_node_lock(&file);
    if (vfs_getattr(file.mnt, file.node, &st) != 0) {
        vfs_node_unlock(&file);
        return -EIO;
    }
    m->sh->size = st.size;
    share = !m->ds->stores;
    if ((uint64_t)pgoff * PAGE_SIZE >= st.size) {
        rc = -ENXIO;
    } else if ((*phys = shmem_resident(m->sh, pgoff)) != 0) {
        frame_ref(*phys);
    } else if (!(cached = share && filecache_lend(&m->entry, pgoff, 1, phys) != 0) &&
               (*phys = alloc_user_frame()) == 0) {
        rc = -ENOMEM;
    } else {
        if (!cached) {
            epoch = filecache_epoch(file.mnt, file.node);
            cached = !share && filecache_read(&m->entry, pgoff, *phys);
        }
        if (!cached) {
            if (m->ds->misses < DISK_MISSES_MOST) {
                m->ds->misses++;
            }
            got = share && m->ds->misses <= DISK_LOTS_MISSED && filecache_keeps(&m->entry)
                      ? disk_read_around(m, pgoff, st.size, phys, epoch)
                      : disk_read_page(m, pgoff, *phys);
        }
        if (got < 0) {
            free_frame(*phys);
            *phys = 0;
            rc = got;
        } else {
            if (!cached && got > 0) {
                if (share) {
                    filecache_keep(&m->entry, pgoff, *phys, epoch);
                } else {
                    filecache_store(&m->entry, pgoff, *phys, epoch);
                }
            }
            *phys = disk_adopt(m, pgoff, *phys);
            rc = !*phys ? -ENOMEM : cached ? 0 : 1;
        }
    }
    if (rc == 0 && m->ds->misses != 0) {
        m->ds->misses--; /* a hit, as do_async_mmap_readahead counts one */
    }
    vfs_node_unlock(&file);
    return rc;
}

/**
 * Let a fault of the disk file of `m` sleep on the file's lock and in its
 * read, as `filemap_read` lets one: a fault outside a system call enters
 * one for the time of the call, with interrupts on, and `disk_call_end`
 * leaves it. 1 when it entered one, with the interrupt state in `*flags`;
 * 0 when the fault may already sleep or the file reads without sleeping;
 * -1 when it may not sleep and the file needs to.
 */
static int disk_call_begin(const struct filemap *m, uint32_t *flags)
{
    if (kirq_enabled() && proc_in_syscall()) {
        return 0;
    }
    if (current_proc && !current_proc->in_syscall) {
        current_proc->in_syscall = 1;
        *flags = kirq_save();
        arch_irq_enable();
        return 1;
    }
    return filemap_fault_readable(m->entry.ofd->vmnt) ? 0 : -1;
}

/** End what `disk_call_begin` began, given its result `entered` and the
 * `flags` it saved. */
static void disk_call_end(int entered, uint32_t flags)
{
    if (entered > 0) {
        kirq_restore(flags);
        current_proc->in_syscall = 0;
    }
}

/** Borrow the file's disk page, enabling sleep across its lock and read
 * when faulting outside a system call, as `filemap_read` does. The caller
 * holds the mapping. Returns 0 for a resident page, 1 after a read, ENXIO
 * past EOF, or another negative errno. */
static int filemap_share_disk(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    struct filemap *m = (struct filemap *)f;
    uint32_t flags = 0;
    int entered = disk_call_begin(m, &flags);
    int rc;

    if (entered < 0) {
        return -EIO;
    }
    rc = filemap_disk_page(m, pgoff, phys);
    disk_call_end(entered, flags);
    return rc;
}

/**
 * The `mkwrite` of a shared mapping of a disk file: mark page `pgoff` as one
 * its stores may reach (`dshare_mark`) before the fault maps it writable,
 * as Linux's do_page_mkwrite and the dirtying of the folio come before a
 * shared mapping's page gains write access (mm/memory.c), under the file's
 * lock, with sleep enabled across it as `filemap_share_disk` enables it.
 * The caller holds the mapping and frame `phys`. Refuse a frame truncation
 * removed or replaced before the lock was taken, or beyond EOF, as Linux
 * checks the folio's mapping in filemap_page_mkwrite. Returns 0, -EAGAIN
 * for a stale frame, -ENOMEM, or -EIO when the fault may not sleep.
 */
static int filemap_mkwrite_disk(struct vm_file *f, uint32_t pgoff, uintptr_t phys)
{
    struct filemap *m = (struct filemap *)f;
    struct vfs_ref file;
    struct vfs_stat st;
    uint32_t flags = 0;
    int entered = disk_call_begin(m, &flags);
    int rc;

    if (entered < 0) {
        return -EIO;
    }
    file.mnt = m->entry.ofd->vmnt;
    file.node = m->entry.ofd->vnode;
    vfs_node_lock(&file);
    rc = vfs_getattr(file.mnt, file.node, &st);
    if (rc == 0) {
        rc = (uint64_t)pgoff * PAGE_SIZE < st.size && shmem_resident(m->sh, pgoff) == phys
                 ? dshare_mark(m->ds, pgoff)
                 : -EAGAIN;
    }
    vfs_node_unlock(&file);
    disk_call_end(entered, flags);
    return rc;
}

/**
 * Lend the pages `first` to `first + count - 1` of the disk file of `f`
 * that are in memory, each in `phys[i]` with a reference for a mapping, and
 * 0 in `phys[i]` for a page that is not, which is left to its own fault and
 * read then: the pages a fault maps around the one it reads, as Linux's
 * filemap_map_pages maps only the folios already in the page cache and
 * reads none (mm/filemap.c:3634-3719). A page the file page cache has
 * counts as in memory while its frame may be shared (`filemap_disk_page`):
 * it joins the file's pages, so that a program started again maps its text
 * sixteen pages a fault, as it would were the pages still mapped. A page
 * past the file's end is in neither, since a truncation takes those out
 * under the file's lock, under which the pages are looked up here, and the
 * cache lends only pages of the file as it is now. At most the first 32 of
 * the range come from the cache. With `hits` set, the pages lent count
 * against the file's misses (`struct dshare`); without it, after the
 * fault's read, they may be pages that read brought in, and count for
 * nothing. Returns the pages lent.
 */
static uint32_t filemap_around_disk(struct vm_file *f, uint32_t first, uint32_t count,
                                    uintptr_t *phys, int hits)
{
    struct filemap *m = (struct filemap *)f;
    struct vfs_ref file;
    uint32_t flags = 0, i, lent = 0, missing = 0;
    int entered = disk_call_begin(m, &flags);

    for (i = 0; i < count; i++) {
        phys[i] = 0;
    }
    if (entered < 0) {
        return 0;
    }
    file.mnt = m->entry.ofd->vmnt;
    file.node = m->entry.ofd->vnode;
    vfs_node_lock(&file);
    for (i = 0; i < count; i++) {
        phys[i] = shmem_resident(m->sh, first + i);
        if (phys[i]) {
            frame_ref(phys[i]);
            lent++;
        } else if (i < 32u) {
            missing |= 1u << i;
        }
    }
    if (missing && !m->ds->stores &&
        filecache_lend(&m->entry, first, count < 32u ? count : 32u, phys) != 0) {
        for (i = 0; i < count && i < 32u; i++) {
            if ((missing & (1u << i)) && phys[i]) {
                phys[i] = disk_adopt(m, first + i, phys[i]);
                lent += phys[i] != 0;
            }
        }
    }
    if (hits) {
        m->ds->misses = m->ds->misses > lent ? m->ds->misses - lent : 0;
    }
    vfs_node_unlock(&file);
    disk_call_end(entered, flags);
    return lent;
}

/**
 * Let go of the mapping `f` with the last region that maps it, and of the
 * copy of the descriptor it keeps, as a close of the descriptor would. A
 * mapping of a file of the KRFFS root is one of the file's holders, which
 * `filemap_holds` finds on the list of mappings, and it leaves the list only
 * once the root's namespace lock is held (`fd_release_lock`): a release of
 * the file's last descriptor elsewhere counts it until then, and the count
 * and any reclaim of the file its own release makes come in the same hold.
 * A mapping of a disk file's pages through a descriptor open for writing
 * writes them back first, before it takes that lock. A sync whose boundary
 * covers this release waits for the write-back to end (`writing`), using
 * a backing-file description of its own. Shared anonymous
 * memory's own pages go last, giving way every leaf
 * (`shmem_free_yielding`): the mapping is off the list and its last region
 * gone by then, and the release runs from `mm_reap`, which holds no
 * regions' lock and whose callers let it sleep in a write-back. A failed
 * write keeps an own file reference, its descriptor, pages and marks for
 * sync to retry, and its error goes to the file system's write-back errors
 * (`dshare_failed`), which each description's sync reports once; a
 * write-back of every page that succeeds, a sync's or another release's,
 * drops that reference (`dshare_retried`) for another release, which a
 * bounded reap leaves for later.
 */
static void filemap_release(struct vm_file *f)
{
    struct filemap *m = (struct filemap *)f;
    struct filemap **link = &maps;
    struct vfs_mount *locked;

    if (m->ds && m->shared && (m->entry.flags & FD_FLAG_WRITE) && m->retained != 2) {
        int rc;

        m->writing = 1;
        rc = dshare_write_back(m->ds, m, 0, m->ds->sh.npages);
        m->writing = 0;
        wq_wake_all(&m->ds->left);
        if (rc != 0) {
            m->retained = 1;
            vm_file_get(f);
            dshare_failed(m, rc);
            klog(KLOG_WARN, "mm", "shared mapping: %s: write-back failed: %d",
                 m->vf.path ? m->vf.path : "a file", rc);
            return;
        }
        (void)dshare_retried(m->ds);
    }
    locked = m->entry.ofd ? fd_release_lock(&m->entry) : NULL;
    while (*link && *link != m) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = m->next;
    }
    if (m->sh) {
        m->sh->maps--;
    }
    if (m->ds) {
        dshare_put(m->ds);
    }
    if (m->entry.ofd) {
        m->entry.node = filemap_node(m);
        fd_release_backing(&m->entry);
    }
    if (locked) {
        vfs_ns_unlock(locked);
    }
    if (m->sh == &m->own) {
        shmem_free_yielding(&m->own);
    }
    kfree(m->vf.path);
    kfree(m);
}

/**
 * Whether the faults of a mapping of the file `e` is open on may read it
 * without the big lock (`struct vm_file`'s `unlocked`): a regular file of a
 * type that reads without it, ext2 (`reads_unlocked`), reached through no
 * device or synthetic node and not the KRFFS root's, whose fill and share
 * then take only the file's lock, the file cache's and the block layer's,
 * as `file_vfs_unlocked` admits a read(2) of it.
 */
static int filemap_reads_unlocked(const fd_entry_t *e)
{
    const struct open_file *ofd = e->ofd;
    const struct vfs_type *t;

    if (e->type != FD_TYPE_FILE || e->node || !ofd || !ofd->vmnt || ofd->dev_mnt ||
        ofd->synth_mnt) {
        return 0;
    }
    t = vfs_mount_base(ofd->vmnt)->type;
    return t && t->reads_unlocked && !t->ns_serialised;
}

struct vm_file *filemap_new(const fd_entry_t *e)
{
    struct filemap *m = kzalloc(sizeof(*m));

    if (!m) {
        return NULL;
    }
    m->vf.refs = 1;
    m->vf.fill = filemap_fill;
    m->vf.release = filemap_release;
    m->vf.unlocked = filemap_reads_unlocked(e);
    m->entry = *e;
    m->entry.flags &= ~(uint32_t)FD_FLAG_CLOEXEC;
    fd_ofd_ref(m->entry.ofd);
    m->next = maps;
    maps = m;
    return &m->vf;
}

struct shmem *filemap_shmem(const struct vm_file *f)
{
    return f && f->release == filemap_release ? ((const struct filemap *)f)->sh : NULL;
}

const struct open_file *filemap_ofd(const struct vm_file *f)
{
    return f && f->release == filemap_release ? ((const struct filemap *)f)->entry.ofd : NULL;
}

uint64_t filemap_anon_ino(const struct vm_file *f)
{
    return f && f->release == filemap_release ? ((const struct filemap *)f)->anon_ino : 0u;
}

const fd_entry_t *filemap_entry(const struct vm_file *f)
{
    if (!f || f->release != filemap_release || !((const struct filemap *)f)->entry.ofd) {
        return NULL;
    }
    return &((const struct filemap *)f)->entry;
}

int filemap_in_memory(const struct vm_file *f)
{
    return f && f->share == filemap_share;
}

struct vm_file *filemap_shared(const fd_entry_t *e, struct shmem *sh)
{
    struct vm_file *f = filemap_new(e);

    if (f) {
        ((struct filemap *)f)->sh = sh;
        ((struct filemap *)f)->shared = 1;
        sh->maps++;
        f->fill = NULL;
        f->share = filemap_share;
        f->unlocked = 0;
    }
    return f;
}

/** A mapping of the disk file of `e`, its file's pages in memory found or
 * made (`dshare_of`), shared when `shared`, which through a descriptor open
 * for writing marks the file as one whose pages may hold stores (`stores`):
 * a shared mapping through one open only for reading can never be made
 * writable (VM_PROT_NOWRITE). NULL when memory runs out. Under the big
 * lock, as mmap makes its backing. */
static struct vm_file *filemap_disk_map(const fd_entry_t *e, int shared)
{
    struct dshare *ds = dshare_of(e->ofd->vmnt, e->ofd->ino);
    struct filemap *m;
    uint32_t flags;
    int made = !ds;

    if (!ds) {
        ds = kzalloc(sizeof(*ds));
        if (!ds) {
            return NULL;
        }
        ds->mnt = vfs_mount_base(e->ofd->vmnt);
        ds->ino = e->ofd->ino;
        wait_queue_init(&ds->left);
        flags = spin_lock_irqsave(&dshare_lock);
        ds->next = *dshare_bucket(ds->ino);
        *dshare_bucket(ds->ino) = ds;
        dshare_count++;
        spin_unlock_irqrestore(&dshare_lock, flags);
    }
    m = (struct filemap *)filemap_new(e);
    if (!m) {
        if (made) {
            dshare_unlink(ds);
            kfree(ds);
        }
        return NULL;
    }
    m->shared = shared;
    m->ds = ds;
    m->sh = &ds->sh;
    if (ds->users++ == 0) {
        ds->clean = tick_count(); /* no store is older than its first mapping */
    }
    ds->sh.maps++;
    if (shared && (e->flags & FD_FLAG_WRITE) && !ds->stores) {
        flags = spin_lock_irqsave(&dshare_lock);
        ds->stores = 1;
        dshare_stores_in[dshare_index(ds->ino)]++;
        spin_unlock_irqrestore(&dshare_lock, flags);
        /* Its stores reach the pages, which may be the cache's frames. */
        filecache_forget_ino(ds->mnt, ds->ino);
    }
    m->vf.fill = NULL;
    m->vf.share = filemap_share_disk;
    m->vf.around = filemap_around_disk;
    m->vf.mkwrite = shared ? filemap_mkwrite_disk : NULL;
    return &m->vf;
}

struct vm_file *filemap_disk_shared(const fd_entry_t *e)
{
    return filemap_disk_map(e, 1);
}

int filemap_disk_pages(const struct vm_file *f)
{
    return f && f->share == filemap_share_disk && ((const struct filemap *)f)->shared;
}

struct vm_file *filemap_private(const fd_entry_t *e, struct shmem *sh, int disk)
{
    struct vm_file *f;

    if (disk && !sh) {
        return filemap_disk_map(e, 0);
    }
    if (sh) {
        f = filemap_shared(e, sh);
    } else {
        return filemap_new(e);
    }
    if (f) {
        ((struct filemap *)f)->shared = 0;
    }
    return f;
}

int filemap_disk_shared_mapped(const struct vfs_mount *mnt, uint64_t ino)
{
    const struct dshare *ds = dshare_of(mnt, ino);
    const struct filemap *m;

    for (m = maps; ds && m; m = m->next) {
        if (m->ds == ds && m->shared) {
            return 1;
        }
    }
    return 0;
}

int filemap_memory_shared_writable(const struct vfs_mount *mnt, void *node)
{
    const struct shmem *sh;
    const struct filemap *m;

    if (mnt->type != &vfs_tmpfs_type || !(sh = tmpfs_shmem(node))) {
        return 0;
    }
    if (tmpfs_seals(node) & TMPFS_SEAL_SHRINK) {
        return 1;
    }
    for (m = maps; m; m = m->next) {
        if (m->sh == sh && m->shared && (m->entry.flags & FD_FLAG_WRITE)) {
            return 1;
        }
    }
    return 0;
}

int filemap_disk_stores(const struct vfs_mount *mnt, uint64_t ino)
{
    const struct dshare *ds;
    uint32_t flags;
    int stores = 0;

    if (*(volatile uint32_t *)&dshare_stores_in[dshare_index(ino)] == 0) {
        return 0;
    }
    mnt = vfs_mount_base(mnt);
    flags = spin_lock_irqsave(&dshare_lock);
    for (ds = *dshare_bucket(ino); ds && !(ds->mnt == mnt && ds->ino == ino); ds = ds->next) {
    }
    stores = ds && ds->stores;
    spin_unlock_irqrestore(&dshare_lock, flags);
    return stores;
}

int filemap_disk_mapped(const struct vfs_mount *mnt, uint64_t ino)
{
    const struct dshare *ds;
    uint32_t flags;

    if (*(volatile uint32_t *)&dshare_count == 0) {
        return 0;
    }
    mnt = vfs_mount_base(mnt);
    flags = spin_lock_irqsave(&dshare_lock);
    for (ds = *dshare_bucket(ino); ds && !(ds->mnt == mnt && ds->ino == ino); ds = ds->next) {
    }
    spin_unlock_irqrestore(&dshare_lock, flags);
    return ds != NULL;
}

int32_t filemap_read_resident(const struct vfs_mount *mnt, uint64_t ino, uint64_t off,
                              char __user *dst, uint32_t count)
{
    struct dshare *ds = dshare_of(mnt, ino);
    uint32_t in = (uint32_t)(off % PAGE_SIZE);
    uintptr_t phys;
    uint8_t *page;
    int rc;

    if (!ds || count == 0 || off / PAGE_SIZE >= ds->sh.npages) {
        return 0;
    }
    phys = shmem_resident(&ds->sh, (uint32_t)(off / PAGE_SIZE));
    if (!phys) {
        return 0;
    }
    if (count > PAGE_SIZE - in) {
        count = PAGE_SIZE - in;
    }
    frame_ref(phys);
    page = kmap_temp(phys);
    if (!page) {
        free_frame(phys);
        return -ENOMEM;
    }
    rc = copy_to_user(dst, page + in, count);
    kunmap_temp(page);
    free_frame(phys);
    return rc == 0 ? (int32_t)count : -EFAULT;
}

void filemap_file_written(fd_entry_t *e, uint64_t off, uint32_t len)
{
    struct dshare *ds = e->ofd ? dshare_of(e->ofd->vmnt, e->ofd->ino) : NULL;
    uint64_t stop = off + len;
    uint8_t buf[512];
    uint32_t i;

    if (!ds || len == 0) {
        return;
    }
    ds->users++;
    for (i = (uint32_t)(off / PAGE_SIZE); (uint64_t)i * PAGE_SIZE < stop; i++) {
        uint64_t base = (uint64_t)i * PAGE_SIZE;
        uint64_t from = off > base ? off : base;
        uint64_t to = stop < base + PAGE_SIZE ? stop : base + PAGE_SIZE;
        int32_t got;
        uint8_t *page;

        if (!shmem_resident(&ds->sh, i)) {
            continue;
        }
        while (from < to) {
            uint32_t take = to - from > sizeof(buf) ? sizeof(buf) : (uint32_t)(to - from);

            got = fs_read_at(e, buf, take, from);
            if (got <= 0 || !shmem_resident(&ds->sh, i)) {
                break;
            }
            page = kmap_temp(shmem_resident(&ds->sh, i));
            if (page) {
                k_memcpy(page + (from - base), buf, (uint32_t)got);
                kunmap_temp(page);
            }
            from += (uint32_t)got;
        }
    }
    dshare_put(ds);
}

void filemap_disk_written(const struct vfs_mount *mnt, uint64_t ino, uint64_t off, const void *buf,
                          uint32_t len)
{
    struct dshare *ds = dshare_of(mnt, ino);
    const uint8_t *in = buf;

    while (ds && len) {
        uint32_t at = (uint32_t)(off % PAGE_SIZE);
        uint32_t take = PAGE_SIZE - at;
        uintptr_t phys = shmem_resident(&ds->sh, (uint32_t)(off / PAGE_SIZE));
        uint8_t *page = phys ? kmap_temp(phys) : NULL;

        if (take > len) {
            take = len;
        }
        if (page) {
            k_memcpy(page + at, in, take);
            kunmap_temp(page);
        }
        in += take;
        off += take;
        len -= take;
    }
}

void filemap_file_truncated(const struct vfs_mount *mnt, uint64_t ino, uint64_t size)
{
    struct dshare *ds = dshare_of(mnt, ino);

    if (ds) {
        (void)shmem_truncate(&ds->sh, size);
        dshare_unmark_from(ds, (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE));
    }
}

int filemap_sync_file(const struct open_file *ofd)
{
    struct dshare *ds = dshare_of(ofd->vmnt, ofd->ino);
    int rc;

    if (!ds) {
        return 0;
    }
    ds->users++;
    rc = dshare_sync(ds, 0, 0xFFFFFFFFu, NULL, 0);
    dshare_put(ds);
    return rc;
}

void filemap_sync_all(void)
{
    struct dshare *ds, *next;
    uint32_t i;

    for (i = 0; i < DSHARE_BUCKETS; i++) {
        for (ds = dshares[i]; ds; ds = next) {
            ds->users++;
            (void)dshare_sync(ds, 0, 0xFFFFFFFFu, NULL, 0);
            next = ds->next;
            dshare_put(ds);
        }
    }
}

int filemap_msync(struct vm_file *f, int flush)
{
    struct filemap *m = (struct filemap *)f;

    return f->share == filemap_share_disk ? dshare_sync(m->ds, 0, 0xFFFFFFFFu, m, flush) : 0;
}

/* The name /proc/PID/maps gives shared anonymous memory, Linux's for the
 * shmem file shmem_zero_setup makes (mm/shmem.c:5383). */
static const char anon_shared_name[] = "/dev/zero (deleted)";

struct vm_file *filemap_anon_shared(void)
{
    struct filemap *m = kzalloc(sizeof(*m));

    if (!m) {
        return NULL;
    }
    m->vf.refs = 1;
    m->vf.share = filemap_share;
    m->vf.release = filemap_release;
    if ((m->vf.path = kmalloc(sizeof(anon_shared_name))) != NULL) {
        k_memcpy(m->vf.path, anon_shared_name, sizeof(anon_shared_name));
    }
    m->sh = &m->own;
    m->own.maps = 1;
    m->anon_ino = tmpfs_new_ino(vfs_memfd_mount());
    m->next = maps;
    maps = m;
    return &m->vf;
}

/** What `filemap_shmem_cut` unmaps: the pages of `sh` from `first`. */
struct shmem_cut {
    const struct shmem *sh;
    uint32_t first;
};

/** The `paging_zap_where` match of `filemap_shmem_cut`: the part of the
 * region `r` that maps the cut pages of `arg`, a `struct shmem_cut`. */
static int shmem_cut_match(const vma_t *r, void *arg, uintptr_t *lo, uintptr_t *hi)
{
    const struct shmem_cut *c = arg;
    uintptr_t pages = (r->end - r->start) / PAGE_SIZE;

    if (!r->file || (r->file->share != filemap_share && r->file->share != filemap_share_disk) ||
        ((const struct filemap *)r->file)->sh != c->sh ||
        (r->pgoff < c->first && pages <= c->first - r->pgoff)) {
        return 0;
    }
    *lo = r->pgoff < c->first ? r->start + (uintptr_t)(c->first - r->pgoff) * PAGE_SIZE : r->start;
    *hi = r->end;
    return 1;
}

void filemap_shmem_cut(struct shmem *sh, uint32_t first)
{
    struct shmem_cut c;

    c.sh = sh;
    c.first = first;
    paging_zap_where(shmem_cut_match, &c);
}

int filemap_holds(const void *node)
{
    const struct filemap *m;

    for (m = maps; m; m = m->next) {
        if (node && filemap_node(m) == node) {
            return 1;
        }
    }
    return 0;
}
