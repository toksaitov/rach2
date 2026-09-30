/*
 * filemap.c: the files mmap maps, as filemap.h describes them.
 */

#include "filemap.h"

#include "kheap.h"
#include "mm.h"
#include "paging.h"
#include "shmem.h"
#include "../isr.h"
#include "../fs/krffs.h"
#include "../fs/vfs.h"
#include "../proc/process.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../lib/kuser.h"
#include "../network/socket/socket.h"
#include "../syscall_internal.h"

/*
 * A file on a disk that shared mappings map: its pages in memory, which
 * every such mapping of the file maps, so that a store through one is seen
 * through all. A page is read from the file at its first touch and written
 * back whole, up to the file's size, which a mapping does not change. Every
 * page in memory is written back, since the kernel does not ask the page
 * tables which pages were stored to. It is found by its mount and inode
 * number, as the calls on the file's descriptors find it.
 */
struct dshare {
    struct dshare *next;
    const struct vfs_mount *mnt;
    uint64_t ino;
    uint32_t users; /* the mappings of it */
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
    int going; /* being released, and no writer for its file's pages */
};

static struct filemap *maps;
static struct dshare *dshares;

static int filemap_fill(struct vm_file *f, uint32_t pgoff, uintptr_t phys)
{
    struct filemap *m = (struct filemap *)f;
    uint8_t *page = kmap_temp(phys);
    int32_t got;

    if (!page) {
        return -1;
    }
    got = fs_read_at(&m->entry, page, PAGE_SIZE, (uint64_t)pgoff * PAGE_SIZE);
    if (got >= 0 && got < (int32_t)PAGE_SIZE) {
        k_memset(page + got, 0, PAGE_SIZE - (uint32_t)got);
    }
    kunmap_temp(page);
    return got < 0 ? -1 : 0;
}

static uintptr_t filemap_share(struct vm_file *f, uint32_t pgoff)
{
    return shmem_map_page(((struct filemap *)f)->sh, pgoff);
}

/* The KRFFS root node the mapping's file is now, or NULL for a file of
 * another type, whose copy of the descriptor has no node. The copy keeps
 * the node the file was opened on, which a relocation leaves behind; the
 * description's is the one `vfs_repoint` moves. */
static void *filemap_node(const struct filemap *m)
{
    return m->entry.type == FD_TYPE_FILE && m->entry.node && m->entry.ofd ? m->entry.ofd->vnode
                                                                          : NULL;
}

static struct dshare *dshare_of(const struct vfs_mount *mnt, uint64_t ino)
{
    struct dshare *ds;

    for (ds = dshares; ds && !(ds->mnt == mnt && ds->ino == ino); ds = ds->next) {
    }
    return ds;
}

/** A mapping of `ds` through a descriptor open for writing, whose copy of
 * the descriptor writes the pages back; NULL when none is left. */
static struct filemap *dshare_writer(const struct dshare *ds)
{
    struct filemap *m;

    for (m = maps; m && !(m->ds == ds && !m->going && (m->entry.flags & FD_FLAG_WRITE));
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
 * Write the pages of `ds` from page `first` up to page `end` back through
 * `m`'s copy of the descriptor, each by way of a buffer from the heap, since
 * the write sleeps. A page is looked up again after each write, which a
 * truncation or a fault may have changed. Returns 0 or the first failed
 * write's error.
 */
static int dshare_write_back(struct dshare *ds, struct filemap *m, uint32_t first, uint32_t end)
{
    uint8_t *buf;
    struct vfs_stat st;
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
        uint64_t at = (uint64_t)i * PAGE_SIZE;
        uintptr_t phys = shmem_resident(&ds->sh, i);
        uint32_t len;
        uint8_t *page;
        int32_t n;

        if (!phys || vfs_getattr(m->entry.ofd->vmnt, m->entry.ofd->vnode, &st) != 0 ||
            at >= st.size) {
            continue;
        }
        len = st.size - at < PAGE_SIZE ? (uint32_t)(st.size - at) : PAGE_SIZE;
        page = kmap_temp(phys);
        if (!page) {
            rc = rc ? rc : -EIO;
            continue;
        }
        k_memcpy(buf, page, len);
        kunmap_temp(page);
        n = fs_write_at(&m->entry, buf, len, at);
        if (n != (int32_t)len && rc == 0) {
            rc = n < 0 ? n : -EIO;
        }
    }
    kfree(buf);
    return rc;
}

/**
 * Write the pages of `ds` from page `first` up to page `end` back through a
 * mapping that may write the file, holding both across the writes, which
 * sleep. Returns 0 or the first failed write's error.
 */
static int dshare_sync(struct dshare *ds, uint32_t first, uint32_t end)
{
    struct filemap *m = dshare_writer(ds);
    int rc;

    if (!m) {
        return 0;
    }
    vm_file_get(&m->vf);
    ds->users++;
    rc = dshare_write_back(ds, m, first, end);
    dshare_put(ds);
    vm_file_put(&m->vf);
    return rc;
}

/**
 * The share of a shared mapping of a disk file: the page in memory, read
 * from the file first when it is not there. The read may sleep, and a page
 * another fault read meanwhile is the one used (`shmem_adopt`); the fault
 * looks the region up again after it (`shared_fault`).
 */
static uintptr_t filemap_share_disk(struct vm_file *f, uint32_t pgoff)
{
    struct filemap *m = (struct filemap *)f;
    uintptr_t phys = shmem_resident(m->sh, pgoff);

    if (phys) {
        frame_ref(phys);
        return phys;
    }
    phys = alloc_user_frame();
    if (phys == 0) {
        return 0;
    }
    if (filemap_fill(f, pgoff, phys) != 0) {
        free_frame(phys);
        return 0;
    }
    return shmem_adopt(m->sh, pgoff, phys);
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
 * write-back takes it for its writer from then on (`going`).
 */
static void filemap_release(struct vm_file *f)
{
    struct filemap *m = (struct filemap *)f;
    struct filemap **link = &maps;
    struct vfs_mount *locked;

    m->going = 1;
    if (m->ds && (m->entry.flags & FD_FLAG_WRITE)) {
        (void)dshare_write_back(m->ds, m, 0, m->ds->sh.npages);
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
        fd_ofd_unref(m->entry.ofd);
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

struct vm_file *filemap_shared(const fd_entry_t *e, struct shmem *sh)
{
    struct vm_file *f = filemap_new(e);

    if (f) {
        ((struct filemap *)f)->sh = sh;
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
        ds->mnt = e->ofd->vmnt;
        ds->ino = e->ofd->ino;
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
    m->ds = ds;
    m->sh = &ds->sh;
    ds->users++;
    ds->sh.maps++;
    m->vf.fill = NULL;
    m->vf.share = filemap_share_disk;
    return &m->vf;
}

int filemap_disk_mapped(const struct vfs_mount *mnt, uint64_t ino)
{
    return dshare_of(mnt, ino) != NULL;
}

int filemap_file_reading(const struct vfs_mount *mnt, uint64_t ino, uint64_t off, uint32_t len)
{
    struct dshare *ds = dshares ? dshare_of(mnt, ino) : NULL;

    if (!ds || len == 0) {
        return 0;
    }
    return dshare_sync(ds, (uint32_t)(off / PAGE_SIZE),
                       (uint32_t)((off + len + PAGE_SIZE - 1) / PAGE_SIZE));
}

void filemap_file_written(fd_entry_t *e, uint64_t off, uint32_t len)
{
    struct dshare *ds = dshares && e->ofd ? dshare_of(e->ofd->vmnt, e->ofd->ino) : NULL;
    uint32_t i, end;
    uint8_t *buf;

    if (!ds || len == 0) {
        return;
    }
    buf = kmalloc(PAGE_SIZE);
    if (!buf) {
        return;
    }
    ds->users++;
    end = (uint32_t)((off + len + PAGE_SIZE - 1) / PAGE_SIZE);
    for (i = (uint32_t)(off / PAGE_SIZE); i < end; i++) {
        int32_t got;
        uint8_t *page;

        if (!shmem_resident(&ds->sh, i)) {
            continue;
        }
        got = fs_read_at(e, buf, PAGE_SIZE, (uint64_t)i * PAGE_SIZE);
        if (got < 0 || !shmem_resident(&ds->sh, i)) {
            continue;
        }
        k_memset(buf + got, 0, PAGE_SIZE - (uint32_t)got);
        page = kmap_temp(shmem_resident(&ds->sh, i));
        if (page) {
            k_memcpy(page, buf, PAGE_SIZE);
            kunmap_temp(page);
        }
    }
    dshare_put(ds);
    kfree(buf);
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

    return ds ? dshare_sync(ds, 0, 0xFFFFFFFFu) : 0;
}

void filemap_sync_all(void)
{
    struct dshare *ds, *next;

    for (ds = dshares; ds; ds = next) {
        ds->users++;
        (void)dshare_sync(ds, 0, 0xFFFFFFFFu);
        next = ds->next;
        dshare_put(ds);
    }
}

int filemap_msync(struct vm_file *f)
{
    struct filemap *m = (struct filemap *)f;

    return f->share == filemap_share_disk ? dshare_sync(m->ds, 0, 0xFFFFFFFFu) : 0;
}

struct vm_file *filemap_anon_shared(void)
{
    struct filemap *m = kzalloc(sizeof(*m));

    if (!m) {
        return NULL;
    }
    m->vf.refs = 1;
    m->vf.share = filemap_share;
    m->vf.release = filemap_release;
    m->sh = &m->own;
    m->own.maps = 1;
    m->next = maps;
    maps = m;
    return &m->vf;
}

void filemap_shmem_cut(struct shmem *sh, uint32_t first)
{
    process_t *p;
    uint32_t i;

    for (p = proc_list; p; p = p->next) {
        const vm_space_t *vm = p->mm ? &p->mm->vm : NULL;

        for (i = 0; vm && i < vm->count; i++) {
            const vma_t *r = &vm->vmas[i];
            uint32_t pages = (r->end - r->start) / PAGE_SIZE;
            uint32_t from;

            if (r->type != VM_SHARED || !r->file ||
                (r->file->share != filemap_share && r->file->share != filemap_share_disk) ||
                ((const struct filemap *)r->file)->sh != sh ||
                (r->pgoff < first && pages <= first - r->pgoff)) {
                continue;
            }
            from = r->pgoff < first ? r->start + (first - r->pgoff) * PAGE_SIZE : r->start;
            unmap_user_range(p->mm->dir, from, r->end);
        }
    }
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
