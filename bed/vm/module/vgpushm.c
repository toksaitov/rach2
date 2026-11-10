/*
 * vgpushm.c: the VM's module for the test bed's shared-memory GPU transport.
 * It binds QEMU's ivshmem device, maps its shared region for the device
 * backend, delivers its MSI-X vectors to the backend as eventfds and rings
 * the other peer's doorbell; bed/vm/Readme.md describes it.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <linux/atomic.h>
#include <linux/eventfd.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include "vgpushm_uapi.h"

#define IVSHMEM_VENDOR 0x1af4
#define IVSHMEM_DEVICE 0x1110

/* ivshmem's registers in BAR 0, as QEMU's docs/specs/ivshmem-spec.rst has. */
#define REG_IVPOSITION 0x08
#define REG_DOORBELL   0x0c

#define BAR_REGS 0
#define BAR_SHM  2

struct vgpushm;

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
    bool gone;        /* set under ops once the device is unbound */
    atomic_t opened;  /* 1 while a file holds the node; it is exclusive */
    spinlock_t lock;  /* guards efd against the interrupt handlers */
    struct eventfd_ctx *efd[VGPUSHM_VECTORS];
    struct vgpushm_vec vec[VGPUSHM_VECTORS];
    atomic64_t irqs[VGPUSHM_VECTORS];
    atomic64_t signals[VGPUSHM_VECTORS];
    atomic64_t rings;
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
    unsigned int i;

    for (i = 0; i < VGPUSHM_VECTORS; i++)
        vgpushm_set_efd(dev, i, NULL);
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
 * Return: 0, -ENODEV once the device is unbound, or -EINVAL for an offset
 * or a size outside the region.
 */
static int vgpushm_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct vgpushm *dev = file->private_data;
    int err;

    if (vma->vm_pgoff != 0)
        return -EINVAL;
    mutex_lock(&dev->ops);
    err = dev->gone ? -ENODEV : vm_iomap_memory(vma, dev->shm_start, dev->shm_len);
    mutex_unlock(&dev->ops);
    return err;
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
    u32 bell;

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
    dev->regs = pcim_iomap_table(pdev)[BAR_REGS];
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
    unsigned int i;

    misc_deregister(&dev->misc);
    mutex_lock(&dev->ops);
    dev->gone = true;
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
