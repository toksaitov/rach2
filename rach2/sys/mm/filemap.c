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
#include "../proc/wait_queue.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../lib/kuser.h"
#include "filecache.h"
#include "../network/socket/socket.h"
#include "../syscall_internal.h"

/*
 * A disk file's resident pages belong to its private and shared mappings.
 * A shared store reaches every clean mapping; a private store copies first.
 * A page is read on first touch and written back whole, up to the file's
 * size, which a mapping does not change. Shared writers write every page
 * back, since the kernel does not read the page tables' dirty bits. The
 * mount and inode number identify the file to descriptor operations.
 */
struct dshare {
    struct dshare *next;
    const struct vfs_mount *mnt;
    uint64_t ino;
    uint32_t users;   /* the mappings of it */
    uint32_t leaving; /* releases of writers writing its pages back */
    wait_queue_t left;
    struct shmem sh;
};

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
    int shared; /* stores may change the file, and may be written back */
    int going;  /* being released, and no writer for its file's pages */
    /* Shared anonymous memory's inode number on the mount
     * `vfs_memfd_mount` has; 0 for any other. */
    uint64_t anon_ino;
};

static struct filemap *maps;
static struct dshare *dshares;

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

/** The pages in memory of the file `ino` of `mnt`, or NULL when no shared
 * mapping has them. */
static struct dshare *dshare_of(const struct vfs_mount *mnt, uint64_t ino)
{
    struct dshare *ds;

    mnt = vfs_mount_base(mnt);
    for (ds = dshares; ds && !(ds->mnt == mnt && ds->ino == ino); ds = ds->next) {
    }
    return ds;
}

/** A mapping of `ds`, through a descriptor open for writing when `writer`,
 * whose copy of the descriptor writes the pages back or flushes the file;
 * NULL when none is left. A mapping whose last reference has gone is
 * waiting for `vm_file_reap` or being released, and is not taken again. */
static struct filemap *dshare_mapping(const struct dshare *ds, int writer)
{
    struct filemap *m;

    for (m = maps; m && !(m->ds == ds && m->vf.refs > 0 && !m->going &&
                          (!writer || (m->shared && (m->entry.flags & FD_FLAG_WRITE))));
         m = m->next) {
    }
    return m;
}

/** Let go of a hold on `ds`, a mapping's or a write-back's, and free it
 * with the last. Nothing here sleeps. */
static void dshare_put(struct dshare *ds)
{
    struct dshare **link = &dshares;

    if (--ds->users != 0) {
        return;
    }
    while (*link && *link != ds) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = ds->next;
    }
    shmem_free(&ds->sh);
    kfree(ds);
}

/**
 * Write page `i` of `ds` back through `m`'s copy of the descriptor, by way
 * of `buf`, a page from the heap, since the write sleeps. The file's lock
 * (`vfs_node_lock`: the mount's namespace lock, or the file's own on a type
 * that sets `file_locked`) is held from before the file's size is read to
 * the end of the write, so a truncation, which takes it too, runs wholly
 * before or after: the write never reaches past the size it leaves, and the
 * page, looked up after the size is read and copied with nothing between that
 * sleeps, is one a truncation has not freed. A file a loop device stands on
 * is not written, as write(2) does not write it. 0, -EBUSY, or the write's
 * error.
 */
static int dshare_write_page(struct dshare *ds, struct filemap *m, uint32_t i, uint8_t *buf)
{
    struct vfs_mount *mnt = m->entry.ofd->vmnt;
    uint64_t at = (uint64_t)i * PAGE_SIZE;
    struct vfs_ref file;
    struct vfs_stat st;
    uintptr_t phys;
    uint8_t *page;
    uint32_t len;
    int32_t n;
    int rc = 0;

    file.mnt = mnt;
    file.node = m->entry.ofd->vnode;
    vfs_node_lock(&file);
    if (loop_backs(mnt, m->entry.ofd->vnode)) {
        rc = -EBUSY;
    } else if (vfs_getattr(mnt, m->entry.ofd->vnode, &st) != 0) {
        rc = -EIO;
    } else if (at < st.size && (phys = shmem_resident(&ds->sh, i)) != 0) {
        page = kmap_temp(phys);
        if (!page) {
            rc = -EIO;
        } else {
            len = st.size - at < PAGE_SIZE ? (uint32_t)(st.size - at) : PAGE_SIZE;
            k_memcpy(buf, page, len);
            kunmap_temp(page);
            n = fs_write_at(&m->entry, buf, len, at);
            if (n != (int32_t)len) {
                rc = n < 0 ? n : -EIO;
            }
        }
    }
    vfs_node_unlock(&file);
    return rc;
}

/** Write the pages of `ds` from page `first` up to page `end` back through
 * `m`'s copy of the descriptor (`dshare_write_page`). Returns 0, also when
 * `m` has no description, -ENOMEM when no buffer can be had, or the first
 * failure. */
static int dshare_write_back(struct dshare *ds, struct filemap *m, uint32_t first, uint32_t end)
{
    uint8_t *buf;
    uint32_t i;
    int rc = 0;

    if (!m || !m->entry.ofd || !m->entry.ofd->vmnt) {
        return 0;
    }
    buf = kmalloc(PAGE_SIZE);
    if (!buf) {
        return -ENOMEM;
    }
    for (i = first; i < end && i < ds->sh.npages; i++) {
        int page_rc = shmem_resident(&ds->sh, i) ? dshare_write_page(ds, m, i, buf) : 0;

        if (page_rc != 0 && rc == 0) {
            rc = page_rc;
        }
    }
    kfree(buf);
    return rc;
}

/** The condition `dshare_sync` waits for: no writer's release is writing
 * the pages of `arg`, a dshare, back. */
static int dshare_settled(void *arg)
{
    return ((const struct dshare *)arg)->leaving == 0;
}

/**
 * Write the pages of `ds` from page `first` up to page `end` back through a
 * mapping that may write the file, and with `flush` run the file system's
 * fsync of the file after them, as MS_SYNC asks, through any mapping of it,
 * since a write-back of a mapping gone since may have left the file system
 * blocks to put out. A writer being released writes its pages back a page
 * at a time, and no other write-back may use it, so this waits for every
 * such release to finish first: the file then has every store made before
 * the call. The mapping used is held across the writes, which sleep.
 * Returns 0 or the first failure.
 */
static int dshare_sync(struct dshare *ds, uint32_t first, uint32_t end, int flush)
{
    struct filemap *m;
    int rc = 0;

    ds->users++;
    if (ds->leaving) {
        (void)wq_wait_event_nointr(&ds->left, dshare_settled, ds);
    }
    m = dshare_mapping(ds, 1);
    if (!m && flush) {
        m = dshare_mapping(ds, 0);
    }
    if (m) {
        vm_file_get(&m->vf);
        if (m->shared && (m->entry.flags & FD_FLAG_WRITE)) {
            rc = dshare_write_back(ds, m, first, end);
        }
        if (rc == 0 && flush) {
            rc = vfs_fsync(m->entry.ofd->vmnt, m->entry.ofd->fhandle);
        }
        vm_file_put(&m->vf);
    }
    dshare_put(ds);
    return rc;
}

/** Read or borrow disk page `pgoff` under its file's lock, which excludes
 * a write or truncation until the page joins the file's resident pages.
 * Returns 0 with a resident mapping reference in `phys`, 1 after reading
 * the file, or a negative errno. */
static int filemap_disk_page(struct filemap *m, uint32_t pgoff, uintptr_t *phys)
{
    struct vfs_ref file;
    struct vfs_stat st;
    uint8_t *page;
    int32_t got;
    int rc = 0;

    file.mnt = m->entry.ofd->vmnt;
    file.node = m->entry.ofd->vnode;
    vfs_node_lock(&file);
    if (vfs_getattr(file.mnt, file.node, &st) != 0) {
        vfs_node_unlock(&file);
        return -EIO;
    }
    m->sh->size = st.size;
    if ((uint64_t)pgoff * PAGE_SIZE >= st.size) {
        rc = -ENXIO;
    } else if ((*phys = shmem_resident(m->sh, pgoff)) != 0) {
        frame_ref(*phys);
    } else if ((*phys = alloc_user_frame()) == 0) {
        rc = -ENOMEM;
    } else {
        page = kmap_temp(*phys);
        got = page ? filemap_read(m, page, pgoff) : -EIO;
        if (got >= 0 && got < (int32_t)PAGE_SIZE) {
            k_memset(page + got, 0, PAGE_SIZE - (uint32_t)got);
        }
        if (page) {
            kunmap_temp(page);
        }
        if (got < 0) {
            free_frame(*phys);
            *phys = 0;
            rc = got;
        } else {
            *phys = shmem_adopt(m->sh, pgoff, *phys);
            rc = *phys ? 1 : -ENOMEM;
        }
    }
    vfs_node_unlock(&file);
    return rc;
}

/** Borrow the file's disk page, enabling sleep across its lock and read
 * when faulting outside a system call, as `filemap_read` does. The caller
 * holds the mapping. Returns 0 for a resident page, 1 after a read, ENXIO
 * past EOF, or another negative errno. */
static int filemap_share_disk(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    struct filemap *m = (struct filemap *)f;
    const struct vfs_mount *mnt = m->entry.ofd->vmnt;
    uint32_t flags = 0;
    int entered = 0;
    int rc;

    if (!(kirq_enabled() && proc_in_syscall())) {
        if (current_proc && !current_proc->in_syscall) {
            current_proc->in_syscall = 1;
            flags = kirq_save();
            arch_irq_enable();
            entered = 1;
        } else if (!filemap_fault_readable(mnt)) {
            return -EIO;
        }
    }
    rc = filemap_disk_page(m, pgoff, phys);
    if (entered) {
        kirq_restore(flags);
        current_proc->in_syscall = 0;
    }
    return rc;
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
 * writes them back first, before it takes that lock, and no other
 * write-back takes it for its writer from then on (`going`); a sync of the
 * file waits for the write-back to end (`leaving`).
 */
static void filemap_release(struct vm_file *f)
{
    struct filemap *m = (struct filemap *)f;
    struct filemap **link = &maps;
    struct vfs_mount *locked;

    m->going = 1;
    if (m->ds && m->shared && (m->entry.flags & FD_FLAG_WRITE)) {
        int rc;

        m->ds->leaving++;
        rc = dshare_write_back(m->ds, m, 0, m->ds->sh.npages);
        if (--m->ds->leaving == 0) {
            wq_wake_all(&m->ds->left);
        }
        if (rc != 0) {
            klog(KLOG_WARN, "mm", "shared mapping: %s: write-back failed: %d",
                 m->vf.path ? m->vf.path : "a file", rc);
        }
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
        shmem_free(&m->own);
    }
    kfree(m->vf.path);
    kfree(m);
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
    }
    return f;
}

struct vm_file *filemap_disk_shared(const fd_entry_t *e)
{
    struct dshare *ds = dshare_of(e->ofd->vmnt, e->ofd->ino);
    struct filemap *m;

    if (!ds) {
        ds = kzalloc(sizeof(*ds));
        if (!ds) {
            return NULL;
        }
        ds->mnt = vfs_mount_base(e->ofd->vmnt);
        ds->ino = e->ofd->ino;
        wait_queue_init(&ds->left);
        ds->next = dshares;
        dshares = ds;
    }
    m = (struct filemap *)filemap_new(e);
    if (!m) {
        if (ds->users == 0) {
            dshares = ds->next;
            kfree(ds);
        }
        return NULL;
    }
    m->shared = 1;
    m->ds = ds;
    m->sh = &ds->sh;
    ds->users++;
    ds->sh.maps++;
    m->vf.fill = NULL;
    m->vf.share = filemap_share_disk;
    return &m->vf;
}

int filemap_disk_pages(const struct vm_file *f)
{
    return f && f->share == filemap_share_disk && ((const struct filemap *)f)->shared;
}

struct vm_file *filemap_private(const fd_entry_t *e, struct shmem *sh, int disk)
{
    struct vm_file *f;

    if (sh) {
        f = filemap_shared(e, sh);
    } else if (disk) {
        f = filemap_disk_shared(e);
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

int filemap_disk_mapped(const struct vfs_mount *mnt, uint64_t ino)
{
    return dshare_of(mnt, ino) != NULL;
}

int32_t filemap_read_resident(const struct vfs_mount *mnt, uint64_t ino, uint64_t off,
                              char __user *dst, uint32_t count)
{
    struct dshare *ds = dshares ? dshare_of(mnt, ino) : NULL;
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
    struct dshare *ds = dshares && e->ofd ? dshare_of(e->ofd->vmnt, e->ofd->ino) : NULL;
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
    struct dshare *ds = dshares ? dshare_of(mnt, ino) : NULL;

    if (ds) {
        (void)shmem_truncate(&ds->sh, size);
    }
}

int filemap_sync_file(const struct vfs_mount *mnt, uint64_t ino)
{
    struct dshare *ds = dshares ? dshare_of(mnt, ino) : NULL;

    return ds ? dshare_sync(ds, 0, 0xFFFFFFFFu, 0) : 0;
}

void filemap_sync_all(void)
{
    struct dshare *ds, *next;

    for (ds = dshares; ds; ds = next) {
        ds->users++;
        (void)dshare_sync(ds, 0, 0xFFFFFFFFu, 0);
        next = ds->next;
        dshare_put(ds);
    }
}

int filemap_msync(struct vm_file *f, int flush)
{
    struct filemap *m = (struct filemap *)f;

    return f->share == filemap_share_disk ? dshare_sync(m->ds, 0, 0xFFFFFFFFu, flush) : 0;
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
