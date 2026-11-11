/*
 * vgpushm.c: the VM's module for the test bed's shared-memory GPU transport.
 * It binds QEMU's ivshmem device, maps its shared region for the device
 * backend, delivers its MSI-X vectors to the backend as eventfds, rings
 * the other peer's doorbell, and pins the backend's GPU buffers and reports
 * their pages for the driver to map; bed/vm/Readme.md describes it.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <linux/atomic.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "vgpushm_uapi.h"

#define IVSHMEM_VENDOR 0x1af4
#define IVSHMEM_DEVICE 0x1110

/* ivshmem's registers in BAR 0, as QEMU's docs/specs/ivshmem-spec.rst has. */
#define REG_IVPOSITION 0x08
#define REG_DOORBELL   0x0c

#define BAR_REGS 0
#define BAR_SHM  2

/* The driver's block in the region, as rach2/sys/virtio/vgpushm.h lays it
 * out: its epoch and state, and the state of a driver that has detached. */
#define REGION_DRIVER_EPOCH 256
#define REGION_DRIVER_STATE 260
#define DRIVER_GONE         3

/* How often pins kept after their file closed are looked at again. */
#define KEPT_POLL_MS 100

struct vgpushm;

/*
 * One pinned buffer: its pages' addresses in order, and what holds them, a
 * reference on each page of a shmem file or the dma-buf's mapping for the
 * device. `epoch` is the driver's epoch it was pinned for: the driver may
 * map its pages until it leaves that epoch, so a pin whose file closes
 * before is kept until then.
 */
struct vgpushm_pinrec {
    struct list_head node;
    u32 handle;
    u32 epoch;
    u32 kind; /* VGPUSHM_PIN_* */
    unsigned long npages;
    phys_addr_t *phys;
    struct page **pages; /* shmem's, each with a reference */
    struct dma_buf *dmabuf;
    struct dma_buf_attachment *attach;
    struct sg_table *sgt;
};

/* What one vector's interrupt handler is given. */
struct vgpushm_vec {
    struct vgpushm *dev;
    unsigned int n;
};

/* One bound ivshmem device and its misc node, /dev/vgpushmN. */
struct vgpushm {
    struct pci_dev *pdev;
    void __iomem *regs;
    resource_size_t shm_start;
    resource_size_t shm_len;
    unsigned int vectors; /* MSI-X vectors requested; 0 for ivshmem-plain */
    int index;
    char name[16];
    struct miscdevice misc;
    struct kref ref;  /* the binding's and an open file's */
    struct mutex ops; /* orders the ioctls against vgpushm_remove */
    bool gone;        /* set under ops once the device is unbound; mmap reads it without */
    atomic_t opened;  /* 1 while a file holds the node; it is exclusive */
    spinlock_t lock;  /* guards efd against the interrupt handlers */
    struct eventfd_ctx *efd[VGPUSHM_VECTORS];
    struct vgpushm_vec vec[VGPUSHM_VECTORS];
    atomic64_t irqs[VGPUSHM_VECTORS];
    atomic64_t signals[VGPUSHM_VECTORS];
    atomic64_t rings;
    void __iomem *shm; /* the region, for the driver's epoch */
    /* The pins, under ops: the open file's, and those kept after it closed
     * while the driver may still map them, which kept_work looks at. */
    struct list_head pins;
    struct list_head kept;
    struct delayed_work kept_work;
    u32 next_handle;
    u64 npins, npinned, nkept, made, refused;
};

static DEFINE_IDA(vgpushm_ida);

/**
 * vgpushm_free() - Free a device once neither its binding nor a file has it.
 * @ref: the device's kref
 */
static void vgpushm_free(struct kref *ref)
{
    struct vgpushm *dev = container_of(ref, struct vgpushm, ref);

    ida_free(&vgpushm_ida, dev->index);
    kfree(dev);
}

/**
 * vgpushm_irq() - Signal the eventfd bound to a vector, if any.
 * @irq: the vector's Linux interrupt
 * @arg: the vector's struct vgpushm_vec
 *
 * Context: hard interrupt; takes the device's lock.
 * Return: IRQ_HANDLED, since an MSI-X vector is never shared.
 */
static irqreturn_t vgpushm_irq(int irq, void *arg)
{
    struct vgpushm_vec *vec = arg;
    struct vgpushm *dev = vec->dev;
    unsigned long flags;

    atomic64_inc(&dev->irqs[vec->n]);
    spin_lock_irqsave(&dev->lock, flags);
    if (dev->efd[vec->n]) {
        eventfd_signal(dev->efd[vec->n]);
        atomic64_inc(&dev->signals[vec->n]);
    }
    spin_unlock_irqrestore(&dev->lock, flags);
    return IRQ_HANDLED;
}

/**
 * vgpushm_set_efd() - Bind an eventfd to a vector, putting the one it had.
 * @dev: the device
 * @n: the vector, below dev->vectors
 * @efd: the eventfd's context, or NULL to unbind
 */
static void vgpushm_set_efd(struct vgpushm *dev, unsigned int n, struct eventfd_ctx *efd)
{
    struct eventfd_ctx *old;
    unsigned long flags;

    spin_lock_irqsave(&dev->lock, flags);
    old = dev->efd[n];
    dev->efd[n] = efd;
    spin_unlock_irqrestore(&dev->lock, flags);
    if (old)
        eventfd_ctx_put(old);
}

/**
 * vgpushm_unpin() - Let go of a pin's pages and free it.
 * @dev: the device whose counts hold the pin, or NULL for a pin not made
 * @rec: the pin, off every list, whatever part of it was made
 *
 * Each shmem page is marked dirty before its reference goes, as i915 marks
 * its shmem pages: Rach2 writes them through KVM, which leaves no dirty bit
 * in the VM, and reclaim frees a clean shmem page without writing it to
 * swap, so the blob would read back as zeros. The page is locked for the
 * mark, as unpin_user_pages_dirty_lock locks one, since the file is its
 * owner's, who may truncate it meanwhile.
 *
 * Context: holds dev->ops; may sleep.
 */
static void vgpushm_unpin(struct vgpushm *dev, struct vgpushm_pinrec *rec)
{
    unsigned long i;

    if (rec->sgt)
        dma_buf_unmap_attachment_unlocked(rec->attach, rec->sgt, DMA_BIDIRECTIONAL);
    if (rec->attach)
        dma_buf_detach(rec->dmabuf, rec->attach);
    if (rec->dmabuf)
        dma_buf_put(rec->dmabuf);
    for (i = 0; rec->pages && i < rec->npages; i++)
        if (rec->pages[i]) {
            set_page_dirty_lock(rec->pages[i]);
            put_page(rec->pages[i]);
        }
    if (dev) {
        dev->npins--;
        dev->npinned -= rec->npages;
    }
    kvfree(rec->pages);
    kvfree(rec->phys);
    kfree(rec);
}

/**
 * vgpushm_driver_holds() - Whether the driver may still map a pin's pages:
 * it is in the epoch the pin was made for and has not detached.
 * @dev: the device, bound
 * @epoch: the pin's epoch
 */
static bool vgpushm_driver_holds(struct vgpushm *dev, u32 epoch)
{
    return !dev->gone && ioread32(dev->shm + REGION_DRIVER_EPOCH) == epoch &&
           ioread32(dev->shm + REGION_DRIVER_STATE) != DRIVER_GONE;
}

/**
 * vgpushm_kept_work() - Let go of each kept pin the driver has left.
 * @work: the device's kept_work
 *
 * Looks again every KEPT_POLL_MS while any is kept.
 */
static void vgpushm_kept_work(struct work_struct *work)
{
    struct vgpushm *dev = container_of(to_delayed_work(work), struct vgpushm, kept_work);
    struct vgpushm_pinrec *rec, *next;

    mutex_lock(&dev->ops);
    list_for_each_entry_safe(rec, next, &dev->kept, node)
    {
        if (!vgpushm_driver_holds(dev, rec->epoch)) {
            list_del(&rec->node);
            dev->nkept--;
            vgpushm_unpin(dev, rec);
        }
    }
    if (!list_empty(&dev->kept) && !dev->gone)
        schedule_delayed_work(&dev->kept_work, msecs_to_jiffies(KEPT_POLL_MS));
    mutex_unlock(&dev->ops);
}

/**
 * vgpushm_pin_shmem() - Hold the pages of a shmem file, a memfd.
 * @rec: the pin, with npages and phys sized
 * @file: the file
 * @offset: the first byte, page-aligned
 *
 * Return: 0, -EINVAL past the page that holds the file's end, or the error
 * of reading a page.
 */
static int vgpushm_pin_shmem(struct vgpushm_pinrec *rec, struct file *file, u64 offset)
{
    struct address_space *mapping = file->f_mapping;
    struct page *page;
    unsigned long i;

    if (offset + ((u64)rec->npages << PAGE_SHIFT) >
        round_up((u64)i_size_read(file_inode(file)), PAGE_SIZE))
        return -EINVAL;
    rec->pages = kvcalloc(rec->npages, sizeof(*rec->pages), GFP_KERNEL);
    if (!rec->pages)
        return -ENOMEM;
    for (i = 0; i < rec->npages; i++) {
        page = shmem_read_mapping_page(mapping, (pgoff_t)(offset >> PAGE_SHIFT) + i);
        if (IS_ERR(page))
            return PTR_ERR(page);
        rec->pages[i] = page;
        rec->phys[i] = page_to_phys(page);
    }
    rec->kind = VGPUSHM_PIN_SHMEM;
    return 0;
}

/**
 * vgpushm_pin_dmabuf() - Hold the pages of a dma-buf by mapping it for the
 * device, which pins them for as long as the mapping lasts.
 * @dev: the device
 * @rec: the pin, with npages and phys sized and dmabuf held
 * @offset: the first byte, page-aligned
 *
 * The pages are read from the importer's scatterlist, which dma-buf allows
 * importers only for its DMA addresses: under CONFIG_DMABUF_DEBUG it scrambles
 * each entry's page, which would give Rach2 unrelated memory to map, so such
 * a kernel is refused.
 *
 * Return: 0, -EOPNOTSUPP under CONFIG_DMABUF_DEBUG, -EINVAL past the
 * buffer's end or for a part not page-aligned, -EFAULT for memory with no
 * page of RAM, such as a GPU's own, or the error of the attachment.
 */
static int vgpushm_pin_dmabuf(struct vgpushm *dev, struct vgpushm_pinrec *rec, u64 offset)
{
    struct scatterlist *sg;
    unsigned long n = 0, k;
    u64 at = 0, end = offset + ((u64)rec->npages << PAGE_SHIFT);
    unsigned int i;

    if (IS_ENABLED(CONFIG_DMABUF_DEBUG))
        return -EOPNOTSUPP;
    if (end > rec->dmabuf->size)
        return -EINVAL;
    rec->attach = dma_buf_attach(rec->dmabuf, &dev->pdev->dev);
    if (IS_ERR(rec->attach)) {
        int err = PTR_ERR(rec->attach);

        rec->attach = NULL;
        return err;
    }
    rec->sgt = dma_buf_map_attachment_unlocked(rec->attach, DMA_BIDIRECTIONAL);
    if (IS_ERR(rec->sgt)) {
        int err = PTR_ERR(rec->sgt);

        rec->sgt = NULL;
        return err;
    }
    for_each_sgtable_sg(rec->sgt, sg, i)
    {
        struct page *page = sg_page(sg);

        if (!page)
            return -EFAULT;
        if ((sg->offset | sg->length) & ~PAGE_MASK)
            return -EINVAL;
        for (k = 0; k < sg->length >> PAGE_SHIFT; k++, at += PAGE_SIZE) {
            if (at >= offset && at < end)
                rec->phys[n++] = page_to_phys(nth_page(page, k)) + sg->offset;
        }
    }
    if (n != rec->npages)
        return -EINVAL;
    rec->kind = VGPUSHM_PIN_DMABUF;
    return 0;
}

/**
 * vgpushm_pin() - Serve VGPUSHM_IOC_PIN.
 * @dev: the device, bound
 * @arg: the user's struct vgpushm_pin
 *
 * Context: holds dev->ops.
 * Return: 0; -EFAULT; -EINVAL for an empty or unaligned range, one larger
 * than VGPUSHM_PIN_MAX_SIZE, or a descriptor that is neither a shmem file
 * nor a dma-buf; -EBADF for a descriptor not open; the error of pinning;
 * -ENOMEM.
 */
static long vgpushm_pin(struct vgpushm *dev, void __user *arg)
{
    struct vgpushm_pin req;
    struct vgpushm_pinrec *rec;
    struct file *file;
    long err;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;
    if (req.size == 0 || ((req.offset | req.size) & ~PAGE_MASK) ||
        req.size > VGPUSHM_PIN_MAX_SIZE || req.offset + req.size < req.offset) {
        dev->refused++;
        return -EINVAL;
    }
    rec = kzalloc(sizeof(*rec), GFP_KERNEL);
    if (!rec)
        return -ENOMEM;
    rec->npages = req.size >> PAGE_SHIFT;
    rec->epoch = req.epoch;
    rec->phys = kvcalloc(rec->npages, sizeof(*rec->phys), GFP_KERNEL);
    if (!rec->phys) {
        err = -ENOMEM;
        goto fail;
    }
    rec->dmabuf = dma_buf_get(req.fd);
    if (!IS_ERR(rec->dmabuf)) {
        err = vgpushm_pin_dmabuf(dev, rec, req.offset);
    } else {
        rec->dmabuf = NULL;
        file = fget(req.fd);
        err = !file               ? -EBADF
              : !shmem_file(file) ? -EINVAL
                                  : vgpushm_pin_shmem(rec, file, req.offset);
        if (file)
            fput(file);
    }
    if (err)
        goto fail;
    rec->handle = ++dev->next_handle ? dev->next_handle : ++dev->next_handle;
    req.handle = rec->handle;
    req.kind = rec->kind;
    req.pages = rec->npages;
    if (copy_to_user(arg, &req, sizeof(req))) {
        err = -EFAULT;
        goto fail;
    }
    list_add_tail(&rec->node, &dev->pins);
    dev->npins++;
    dev->npinned += rec->npages;
    dev->made++;
    return 0;

fail:
    dev->refused++;
    vgpushm_unpin(NULL, rec);
    return err;
}

/**
 * vgpushm_find() - The open file's pin of a handle, or NULL.
 * @dev: the device
 * @handle: the handle
 */
static struct vgpushm_pinrec *vgpushm_find(struct vgpushm *dev, u32 handle)
{
    struct vgpushm_pinrec *rec;

    list_for_each_entry(rec, &dev->pins, node) if (rec->handle == handle) return rec;
    return NULL;
}

/**
 * vgpushm_pages() - Serve VGPUSHM_IOC_PAGES.
 * @dev: the device, bound
 * @arg: the user's struct vgpushm_pages
 *
 * Context: holds dev->ops.
 * Return: 0; -EFAULT; -ENOENT for no such pin; -EINVAL for a first page
 * past the pin's end, or a `max` of 0 or over VGPUSHM_PAGES_MAX_RUNS.
 */
static long vgpushm_pages(struct vgpushm *dev, void __user *arg)
{
    struct vgpushm_pages req;
    struct vgpushm_pinrec *rec;
    struct vgpushm_run run;
    struct vgpushm_run __user *out;
    unsigned long i;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;
    rec = vgpushm_find(dev, req.handle);
    if (!rec)
        return -ENOENT;
    if (req.first >= rec->npages || req.max == 0 || req.max > VGPUSHM_PAGES_MAX_RUNS)
        return -EINVAL;
    out = u64_to_user_ptr(req.runs);
    req.count = 0;
    for (i = req.first; i < rec->npages && req.count < req.max;) {
        run.addr = rec->phys[i];
        run.pages = 1;
        for (i++; i < rec->npages && rec->phys[i] == run.addr + (run.pages << PAGE_SHIFT); i++)
            run.pages++;
        if (copy_to_user(&out[req.count], &run, sizeof(run)))
            return -EFAULT;
        req.count++;
    }
    req.next = i;
    return copy_to_user(arg, &req, sizeof(req)) ? -EFAULT : 0;
}

/**
 * vgpushm_open() - Open the node; one file at a time holds a device.
 * @inode: the node
 * @file: the file; misc_open has set private_data to the miscdevice
 *
 * Return: 0, or -EBUSY while another file holds it.
 */
static int vgpushm_open(struct inode *inode, struct file *file)
{
    struct vgpushm *dev = container_of(file->private_data, struct vgpushm, misc);

    if (atomic_cmpxchg(&dev->opened, 0, 1) != 0)
        return -EBUSY;
    kref_get(&dev->ref);
    file->private_data = dev;
    return 0;
}

/**
 * vgpushm_release() - Close the node, unbinding every eventfd it bound.
 * @inode: the node
 * @file: the file
 *
 * Return: 0.
 */
static int vgpushm_release(struct inode *inode, struct file *file)
{
    struct vgpushm *dev = file->private_data;
    struct vgpushm_pinrec *rec, *next;
    unsigned int i;

    for (i = 0; i < VGPUSHM_VECTORS; i++)
        vgpushm_set_efd(dev, i, NULL);
    mutex_lock(&dev->ops);
    list_for_each_entry_safe(rec, next, &dev->pins, node)
    {
        list_del(&rec->node);
        if (vgpushm_driver_holds(dev, rec->epoch)) {
            list_add_tail(&rec->node, &dev->kept);
            dev->nkept++;
        } else {
            vgpushm_unpin(dev, rec);
        }
    }
    if (!list_empty(&dev->kept) && !dev->gone)
        schedule_delayed_work(&dev->kept_work, msecs_to_jiffies(KEPT_POLL_MS));
    mutex_unlock(&dev->ops);
    atomic_set(&dev->opened, 0);
    kref_put(&dev->ref, vgpushm_free);
    return 0;
}

/**
 * vgpushm_mmap() - Map the shared region, BAR 2, from offset 0.
 * @file: the file
 * @vma: the mapping, no larger than the region
 *
 * The mapping is write-back: the region is the host's RAM, which QEMU's
 * other peer maps cached, and under KVM the guest's attribute is ignored.
 *
 * Context: holds mmap_lock for write, so it reads dev->gone without dev->ops,
 * under which the ioctls copy to and from user memory and so take mmap_lock.
 * A mapping made as the device goes is one made before, which stays.
 *
 * Return: 0, -ENODEV once the device is unbound, or -EINVAL for an offset
 * or a size outside the region.
 */
static int vgpushm_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct vgpushm *dev = file->private_data;

    if (vma->vm_pgoff != 0)
        return -EINVAL;
    if (READ_ONCE(dev->gone))
        return -ENODEV;
    return vm_iomap_memory(vma, dev->shm_start, dev->shm_len);
}

/**
 * vgpushm_eventfd() - Bind or unbind the eventfd of VGPUSHM_IOC_EVENTFD.
 * @dev: the device
 * @arg: the user's struct vgpushm_eventfd
 *
 * Return: 0, -EFAULT, -EINVAL for a vector the device has not, or the
 * error of eventfd_ctx_fdget for a descriptor that is no eventfd.
 */
static long vgpushm_eventfd(struct vgpushm *dev, void __user *arg)
{
    struct vgpushm_eventfd req;
    struct eventfd_ctx *efd = NULL;

    if (copy_from_user(&req, arg, sizeof(req)))
        return -EFAULT;
    if (req.vector >= dev->vectors)
        return -EINVAL;
    if (req.fd >= 0) {
        efd = eventfd_ctx_fdget(req.fd);
        if (IS_ERR(efd))
            return PTR_ERR(efd);
    }
    vgpushm_set_efd(dev, req.vector, efd);
    return 0;
}

/**
 * vgpushm_counts() - Copy the counters of VGPUSHM_IOC_COUNTS out.
 * @dev: the device
 * @arg: the user's struct vgpushm_counts
 *
 * Return: 0 or -EFAULT.
 */
static long vgpushm_counts(struct vgpushm *dev, void __user *arg)
{
    struct vgpushm_counts c;
    unsigned int i;

    memset(&c, 0, sizeof(c));
    for (i = 0; i < VGPUSHM_VECTORS; i++) {
        c.irqs[i] = atomic64_read(&dev->irqs[i]);
        c.signals[i] = atomic64_read(&dev->signals[i]);
    }
    c.rings = atomic64_read(&dev->rings);
    c.pins = dev->npins;
    c.pinned = dev->npinned;
    c.kept = dev->nkept;
    c.made = dev->made;
    c.refused = dev->refused;
    return copy_to_user(arg, &c, sizeof(c)) ? -EFAULT : 0;
}

/**
 * vgpushm_request() - Serve one of the node's ioctls, vgpushm_uapi.h's.
 * @dev: the device, bound
 * @cmd: the request
 * @arg: its argument
 *
 * Context: holds dev->ops.
 * Return: 0, a request's error, or -ENOTTY for an unknown request.
 */
static long vgpushm_request(struct vgpushm *dev, unsigned int cmd, void __user *arg)
{
    struct vgpushm_info info;
    struct vgpushm_pinrec *rec;
    u32 bell, handle;

    switch (cmd) {
    case VGPUSHM_IOC_INFO:
        info.size = dev->shm_len;
        info.peer = dev->vectors ? ioread32(dev->regs + REG_IVPOSITION) : ~0u;
        info.vectors = dev->vectors;
        return copy_to_user(arg, &info, sizeof(info)) ? -EFAULT : 0;
    case VGPUSHM_IOC_EVENTFD:
        return vgpushm_eventfd(dev, arg);
    case VGPUSHM_IOC_RING:
        if (dev->vectors == 0)
            return -ENODEV;
        if (copy_from_user(&bell, arg, sizeof(bell)))
            return -EFAULT;
        iowrite32(bell, dev->regs + REG_DOORBELL);
        atomic64_inc(&dev->rings);
        return 0;
    case VGPUSHM_IOC_COUNTS:
        return vgpushm_counts(dev, arg);
    case VGPUSHM_IOC_PIN:
        return vgpushm_pin(dev, arg);
    case VGPUSHM_IOC_PAGES:
        return vgpushm_pages(dev, arg);
    case VGPUSHM_IOC_UNPIN:
        if (copy_from_user(&handle, arg, sizeof(handle)))
            return -EFAULT;
        rec = vgpushm_find(dev, handle);
        if (!rec)
            return -ENOENT;
        list_del(&rec->node);
        vgpushm_unpin(dev, rec);
        return 0;
    default:
        return -ENOTTY;
    }
}

/**
 * vgpushm_ioctl() - Serve an ioctl while the device is bound.
 * @file: the file
 * @cmd: the request
 * @uarg: its argument, a user pointer
 *
 * Return: vgpushm_request's result, or -ENODEV once the device is unbound.
 */
static long vgpushm_ioctl(struct file *file, unsigned int cmd, unsigned long uarg)
{
    struct vgpushm *dev = file->private_data;
    long ret;

    mutex_lock(&dev->ops);
    ret = dev->gone ? -ENODEV : vgpushm_request(dev, cmd, (void __user *)uarg);
    mutex_unlock(&dev->ops);
    return ret;
}

static const struct file_operations vgpushm_fops = {
    .owner = THIS_MODULE,
    .open = vgpushm_open,
    .release = vgpushm_release,
    .mmap = vgpushm_mmap,
    .unlocked_ioctl = vgpushm_ioctl,
    .compat_ioctl = compat_ptr_ioctl,
};

/**
 * vgpushm_irqs() - Enable the device's MSI-X vectors and their handlers.
 * @dev: the device
 *
 * An ivshmem-plain device has no MSI-X: it is served with no vectors, and
 * its users poll.
 *
 * Return: 0, or the error of request_irq.
 */
static int vgpushm_irqs(struct vgpushm *dev)
{
    struct pci_dev *pdev = dev->pdev;
    unsigned int i;
    int n, err;

    n = pci_alloc_irq_vectors(pdev, VGPUSHM_VECTORS, VGPUSHM_VECTORS, PCI_IRQ_MSIX);
    if (n < 0) {
        dev->vectors = 0;
        return 0;
    }
    dev->vectors = n;
    for (i = 0; i < dev->vectors; i++) {
        dev->vec[i].dev = dev;
        dev->vec[i].n = i;
        err = request_irq(pci_irq_vector(pdev, i), vgpushm_irq, 0, dev->name, &dev->vec[i]);
        if (err) {
            while (i-- > 0)
                free_irq(pci_irq_vector(pdev, i), &dev->vec[i]);
            pci_free_irq_vectors(pdev);
            dev->vectors = 0;
            return err;
        }
    }
    return 0;
}

/**
 * vgpushm_free_irqs() - Release what vgpushm_irqs took.
 * @dev: the device
 */
static void vgpushm_free_irqs(struct vgpushm *dev)
{
    unsigned int i;

    for (i = 0; i < dev->vectors; i++)
        free_irq(pci_irq_vector(dev->pdev, i), &dev->vec[i]);
    if (dev->vectors)
        pci_free_irq_vectors(dev->pdev);
    dev->vectors = 0;
}

/**
 * vgpushm_probe() - Bind an ivshmem device: map its registers, enable its
 * vectors and register its node.
 * @pdev: the device
 * @id: its entry of the table
 *
 * Return: 0, or the first error met.
 */
static int vgpushm_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct vgpushm *dev;
    int err;

    err = pcim_enable_device(pdev);
    if (err)
        return err;
    err = pcim_iomap_regions(pdev, BIT(BAR_REGS) | BIT(BAR_SHM), "vgpushm");
    if (err)
        return err;
    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;
    dev->index = ida_alloc(&vgpushm_ida, GFP_KERNEL);
    if (dev->index < 0) {
        err = dev->index;
        kfree(dev);
        return err;
    }
    dev->pdev = pdev;
    kref_init(&dev->ref);
    mutex_init(&dev->ops);
    spin_lock_init(&dev->lock);
    INIT_LIST_HEAD(&dev->pins);
    INIT_LIST_HEAD(&dev->kept);
    INIT_DELAYED_WORK(&dev->kept_work, vgpushm_kept_work);
    /* A pin's dma-buf is mapped for the device; its pages may lie anywhere. */
    (void)dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    dev->regs = pcim_iomap_table(pdev)[BAR_REGS];
    dev->shm = pcim_iomap_table(pdev)[BAR_SHM];
    dev->shm_start = pci_resource_start(pdev, BAR_SHM);
    dev->shm_len = pci_resource_len(pdev, BAR_SHM);
    pci_set_master(pdev);
    snprintf(dev->name, sizeof(dev->name), "vgpushm%d", dev->index);
    err = vgpushm_irqs(dev);
    if (err)
        goto fail_ida;
    dev->misc.minor = MISC_DYNAMIC_MINOR;
    dev->misc.name = dev->name;
    dev->misc.fops = &vgpushm_fops;
    dev->misc.parent = &pdev->dev;
    err = misc_register(&dev->misc);
    if (err)
        goto fail_irqs;
    pci_set_drvdata(pdev, dev);
    dev_info(&pdev->dev, "vgpushm: bound: %s, %llu bytes, %u vectors\n", dev->name,
             (unsigned long long)dev->shm_len, dev->vectors);
    return 0;

fail_irqs:
    vgpushm_free_irqs(dev);
fail_ida:
    kref_put(&dev->ref, vgpushm_free);
    return err;
}

/**
 * vgpushm_remove() - Unbind a device: its node goes, then its vectors.
 * @pdev: the device
 *
 * A file still open keeps the structure, whose ioctls and mmap then fail
 * with -ENODEV; a mapping made before stays, as the region's pages do in
 * QEMU until the VM ends.
 */
static void vgpushm_remove(struct pci_dev *pdev)
{
    struct vgpushm *dev = pci_get_drvdata(pdev);
    struct vgpushm_pinrec *rec, *next;
    unsigned int i;

    misc_deregister(&dev->misc);
    mutex_lock(&dev->ops);
    WRITE_ONCE(dev->gone, true);
    mutex_unlock(&dev->ops);
    cancel_delayed_work_sync(&dev->kept_work);
    mutex_lock(&dev->ops);
    list_for_each_entry_safe(rec, next, &dev->kept, node)
    {
        list_del(&rec->node);
        dev->nkept--;
        vgpushm_unpin(dev, rec);
    }
    list_for_each_entry_safe(rec, next, &dev->pins, node)
    {
        list_del(&rec->node);
        vgpushm_unpin(dev, rec);
    }
    mutex_unlock(&dev->ops);
    vgpushm_free_irqs(dev);
    for (i = 0; i < VGPUSHM_VECTORS; i++)
        vgpushm_set_efd(dev, i, NULL);
    kref_put(&dev->ref, vgpushm_free);
}

static const struct pci_device_id vgpushm_ids[] = {
    {PCI_DEVICE(IVSHMEM_VENDOR, IVSHMEM_DEVICE)},
    {0},
};
MODULE_DEVICE_TABLE(pci, vgpushm_ids);

static struct pci_driver vgpushm_driver = {
    .name = "vgpushm",
    .id_table = vgpushm_ids,
    .probe = vgpushm_probe,
    .remove = vgpushm_remove,
};
module_pci_driver(vgpushm_driver);

MODULE_DESCRIPTION("Rach2 test bed: ivshmem region and doorbells for the GPU backend");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(DMA_BUF);
