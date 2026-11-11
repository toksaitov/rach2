/* /dev/udmabuf exports pinned ranges of sealed tmpfs files as dma-bufs. */
#include "dmabuf.h"
#include "../fs/devfs.h"
#include "../fs/tmpfs.h"
#include "../mm/shmem.h"
#include "../mm/paging.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"

/* Linux's default limits, in entries and pages (64 MiB). */
#define UDMABUF_LIST_LIMIT 1024u
#define UDMABUF_PAGE_LIMIT 16384u

struct udmabuf {
    uintptr_t *pages;
    uint32_t count;
};

/** Return one pinned page, without transferring the pin. */
static int udma_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    struct dma_buf *b = obj;
    struct udmabuf *u = b->priv;
    if (pgoff >= u->count)
        return -1;
    *phys = u->pages[pgoff];
    return 0;
}

/** Drop the reference held by a mapping after its final region goes. */
static void udma_map_put(void *obj)
{
    dma_buf_put(obj);
}

static const struct vm_device_ops udma_vm_ops = {udma_page, udma_map_put, NULL};

/** Map the pinned pages shared, retaining the buffer for the mapping. */
static int32_t udma_mmap(struct dma_buf *b, vma_t *vma)
{
    int rc;
    dma_buf_ref(b);
    rc = vm_device_map(vma, &udma_vm_ops, b, 0);
    if (rc)
        dma_buf_put(b);
    return rc;
}

/** Return every acquired page reference, including a partial creation. */
static void udma_free(struct udmabuf *u)
{
    uint32_t i;
    for (i = 0; i < u->count; i++)
        frame_unref(u->pages[i]);
    kfree(u->pages);
    kfree(u);
}

/** Free the exporter's page pins at the last buffer reference. */
static void udma_release(struct dma_buf *b)
{
    udma_free(b->priv);
}

/** Return a pinned physical page to a device importer, or 0 past the end. */
static uintptr_t udma_buf_page(struct dma_buf *b, uint32_t pgoff)
{
    struct udmabuf *u = b->priv;

    return pgoff < u->count ? u->pages[pgoff] : 0;
}

static const struct dma_buf_ops udma_ops = {udma_mmap, udma_release, NULL, udma_buf_page};

/** Validate and pin each range, concatenating its pages into one buffer. */
static int32_t udma_create(uint32_t flags, uint32_t count, struct udmabuf_create_item *items)
{
    struct udmabuf *u;
    struct dma_buf *b;
    fd_entry_t *e;
    struct shmem *sh;
    uint32_t i, j, seals, pages, total = 0;
    uintptr_t phys;
    int32_t rc = -EINVAL;
    if (!count || count > UDMABUF_LIST_LIMIT)
        return -EINVAL;
    for (i = 0; i < count; i++) {
        if ((items[i].offset | items[i].size) & 4095)
            return -EINVAL;
        if ((items[i].size >> 12) > UDMABUF_PAGE_LIMIT - total)
            return -EINVAL;
        total += (uint32_t)(items[i].size >> 12);
    }
    if (!total)
        return -EINVAL;
    u = kzalloc(sizeof(*u));
    if (!u)
        return -ENOMEM;
    u->pages = kmalloc((size_t)total * sizeof(*u->pages));
    if (!u->pages) {
        kfree(u);
        return -ENOMEM;
    }
    for (i = 0; i < count; i++) {
        e = fd_get(current_proc->fds, (int32_t)items[i].memfd);
        if (!e || (e->flags & FD_FLAG_PATH) || !e->ofd || !e->ofd->vmnt ||
            e->ofd->vmnt->type != &vfs_tmpfs_type || !(sh = tmpfs_shmem(e->ofd->vnode))) {
            rc = -EBADFD;
            goto fail;
        }
        seals = tmpfs_seals(e->ofd->vnode);
        if (!(seals & TMPFS_SEAL_SHRINK) || (seals & TMPFS_SEAL_WRITE))
            goto fail;
        if (items[i].offset > sh->size || items[i].size > sh->size - items[i].offset)
            goto fail;
        pages = (uint32_t)(items[i].size >> 12);
        for (j = 0; j < pages; j++) {
            phys = shmem_map_page(sh, (uint32_t)(items[i].offset >> 12) + j);
            if (!phys) {
                rc = -ENOMEM;
                goto fail;
            }
            u->pages[u->count++] = phys;
        }
    }
    b = dma_buf_export((uint64_t)total << 12, &udma_ops, u, NULL);
    if (!b) {
        rc = -ENOMEM;
        goto fail;
    }
    rc = dma_buf_fd(b, flags & UDMABUF_FLAGS_CLOEXEC ? 02000000u : 0);
    dma_buf_put(b);
    return rc;
fail:
    udma_free(u);
    return rc;
}

/** Read one range or a bounded list into kernel memory before pinning. */
static int32_t udma_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct udmabuf_create one;
    struct udmabuf_create_list head;
    struct udmabuf_create_item item, *items;
    size_t bytes;
    int32_t rc;
    (void)e;
    if (request == UDMABUF_CREATE) {
        if (copy_from_user(&one, (void __user *)arg, sizeof(one)))
            return -EFAULT;
        item.memfd = one.memfd;
        item.__pad = 0;
        item.offset = one.offset;
        item.size = one.size;
        return udma_create(one.flags, 1, &item);
    }
    if (request != UDMABUF_CREATE_LIST)
        return -ENOTTY;
    if (copy_from_user(&head, (void __user *)arg, sizeof(head)))
        return -EFAULT;
    if (!head.count || head.count > UDMABUF_LIST_LIMIT)
        return -EINVAL;
    bytes = (size_t)head.count * sizeof(*items);
    if (arg > (uintptr_t)-1 - sizeof(head) || arg + sizeof(head) > (uintptr_t)-1 - bytes)
        return -EFAULT;
    items = kmalloc(bytes);
    if (!items)
        return -ENOMEM;
    if (copy_from_user(items, (void __user *)(arg + sizeof(head)), bytes)) {
        kfree(items);
        return -EFAULT;
    }
    rc = udma_create(head.flags, head.count, items);
    kfree(items);
    return rc;
}

static const struct file_ops udma_file_ops = {NULL, NULL, NULL, udma_ioctl, NULL, NULL, NULL, 0};
static struct devfs_node udma_node = {"udmabuf", 10, 126, 0666, &udma_file_ops, NULL, NULL};

/** Publish the udmabuf misc device during DRM startup. */
int32_t udmabuf_start(void)
{
    return devfs_node_register(&udma_node);
}
