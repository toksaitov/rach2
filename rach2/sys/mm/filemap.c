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
#include "../lib/kstring.h"
#include "../lib/kuser.h"
#include "../network/socket/socket.h"
#include "../syscall_internal.h"

/* The record: the region map's part first, so the one is found from the
 * other, and the copy of the descriptor; for a shared mapping, the pages it
 * maps, and for shared anonymous memory those pages are its own. */
struct filemap {
    struct vm_file vf;
    fd_entry_t entry;
    struct filemap *next;
    struct shmem *sh;
    struct shmem own;
};

static struct filemap *maps;

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
 * another type, whose copy of the descriptor names no node. The copy keeps
 * the node the file was opened on, which a relocation leaves behind; the
 * description's is the one `vfs_repoint` moves. */
static void *filemap_node(const struct filemap *m)
{
    return m->entry.type == FD_TYPE_FILE && m->entry.node && m->entry.ofd ? m->entry.ofd->vnode
                                                                          : NULL;
}

static void filemap_release(struct vm_file *f)
{
    struct filemap *m = (struct filemap *)f;
    struct filemap **link = &maps;

    while (*link && *link != m) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = m->next;
    }
    if (m->sh) {
        m->sh->maps--;
    }
    if (m->entry.ofd) {
        m->entry.node = filemap_node(m);
        fd_release_backing(&m->entry);
        fd_ofd_unref(m->entry.ofd);
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

            if (r->type != VM_SHARED || !r->file || r->file->share != filemap_share ||
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
