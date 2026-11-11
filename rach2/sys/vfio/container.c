/*
 * container.c: the descriptor of /dev/vfio/vfio, VFIO's container, with
 * Linux's Type1 IOMMU driver behind it: the groups set in it share one
 * IOMMU domain, and MAP_DMA pins the caller's memory and maps it there at
 * the IOVA the caller chose.
 */

#include "vfio.h"

#include "../fs/fd.h"
#include "../fs/file.h"
#include "../iommu.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/vm.h"
#include "../mm/usercopy.h"

#define PAGE_4K 0x1000ull

/* The boot option that lets a container be set up on a host whose
 * interrupts are not remapped, Linux's module parameter of the same name. */
#define UNSAFE_INTERRUPTS "vfio_iommu_type1.allow_unsafe_interrupts=1"

/* The most valid IOVA ranges GET_INFO reports. */
#define IOVA_RANGES_MAX 32

static int32_t container_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);
static void container_close(fd_entry_t *e);

const struct file_ops vfio_container_ops = {NULL, NULL, NULL, container_ioctl, container_close,
                                            NULL, NULL, 0};

/** An open of /dev/vfio/vfio: a new container with no group and no IOMMU
 * driver set, the descriptor's object. */
int32_t vfio_container_open(struct devfs_node *node, struct fd_entry *e, int flags)
{
    struct vfio_container *c = (struct vfio_container *)kzalloc(sizeof(*c));

    (void)node;
    (void)flags;
    if (c == NULL) {
        return -ENOMEM;
    }
    c->fd_open = 1;
    e->priv = c;
    return 0;
}

/** The devfn of the device `d`. */
static uint8_t devfn_of(const struct vfio_device *d)
{
    return (uint8_t)(d->pci.device << 3 | d->pci.function);
}

/** Take the devices of `g` back from the container's domain. */
static void group_detach_devices(struct vfio_container *c, struct vfio_group *g)
{
    int i;

    if (!g->attached) {
        return;
    }
    for (i = 0; i < g->ndev; i++) {
        const struct vfio_device *d = &vfio_devices[g->devs[i]];

        iommu_detach(c->domain, d->pci.bus, devfn_of(d));
    }
    g->attached = 0;
}

/** Give every device of `g` to the container's domain: 0, or the first
 * error of `iommu_attach` with none of them given. */
static int group_attach_devices(struct vfio_container *c, struct vfio_group *g)
{
    int i, j, r;

    for (i = 0; i < g->ndev; i++) {
        const struct vfio_device *d = &vfio_devices[g->devs[i]];

        r = iommu_attach(c->domain, d->pci.bus, devfn_of(d));
        if (r != 0) {
            for (j = 0; j < i; j++) {
                d = &vfio_devices[g->devs[j]];
                iommu_detach(c->domain, d->pci.bus, devfn_of(d));
            }
            klog(KLOG_WARN, "vfio", "attach: group %d: %d", g->id, r);
            return r;
        }
    }
    g->attached = 1;
    return 0;
}

/** Remove the mapping `dma` from the domain and from `c`'s list, unpin
 * its pages and free it. Does not sleep, so that a walk of the list or a
 * release of the container stays whole under the big kernel lock, as
 * Linux's Type1 holds iommu->lock across its unpin: a mapping file the pin
 * held waits for `vm_file_reap`, which the operation runs at its end. */
static void dma_remove(struct vfio_container *c, struct vfio_dma *dma)
{
    struct vfio_dma **pp;

    for (pp = &c->dmas; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == dma) {
            *pp = dma->next;
            break;
        }
    }
    if (dma->pin != NULL && pin_live(dma->pin)) {
        (void)iommu_unmap(c->domain, dma->iova, dma->size);
    }
    if (dma->pin != NULL) {
        unpin(dma->pin);
    }
    c->ndma--;
    kfree(dma);
}

/** Drop the Type1 driver of `c`, as Linux's release of it: every mapping,
 * and the domain, which no device is in any longer. */
static void type1_release(struct vfio_container *c)
{
    while (c->dmas != NULL) {
        dma_remove(c, c->dmas);
    }
    if (c->domain != NULL) {
        iommu_domain_free(c->domain);
    }
    c->domain = NULL;
    c->iommu_type = 0;
}

/** Free `c` once its descriptor is closed and no group is in it. */
static void container_maybe_free(struct vfio_container *c)
{
    if (c->fd_open || c->groups != NULL) {
        return;
    }
    type1_release(c);
    kfree(c);
}

/** Put `g` in `c`, its devices in the domain if the driver is set. 0, or
 * the error of the attach. */
int vfio_container_attach(struct vfio_container *c, struct vfio_group *g)
{
    int r;

    if (c->iommu_type != 0) {
        r = group_attach_devices(c, g);
        if (r != 0) {
            return r;
        }
    }
    g->container = c;
    g->next_in_container = c->groups;
    c->groups = g;
    return 0;
}

/** Take `g` out of its container. The last group out releases the driver,
 * as on Linux, and frees a container whose descriptor is closed. The
 * mapped files the pins held are released last, once nothing here uses
 * the container. */
void vfio_container_detach(struct vfio_group *g)
{
    struct vfio_container *c = g->container;
    struct vfio_group **pp;

    if (c == NULL) {
        return;
    }
    group_detach_devices(c, g);
    for (pp = &c->groups; *pp != NULL; pp = &(*pp)->next_in_container) {
        if (*pp == g) {
            *pp = g->next_in_container;
            break;
        }
    }
    g->container = NULL;
    g->next_in_container = NULL;
    if (c->groups == NULL) {
        type1_release(c);
    }
    container_maybe_free(c);
    vm_file_reap();
}

/** The last close of a container's descriptor; the groups in it keep it. */
static void container_close(fd_entry_t *e)
{
    struct vfio_container *c = (struct vfio_container *)e->priv;

    e->priv = NULL;
    if (c != NULL) {
        c->fd_open = 0;
        container_maybe_free(c);
        vm_file_reap();
    }
}

/** VFIO_CHECK_EXTENSION: 1 for what Type1 has, the two driver types and
 * UNMAP_ALL; 0 for the rest, cache coherence and UPDATE_VADDR among them. */
int vfio_container_check_extension(const struct vfio_container *c, uintptr_t ext)
{
    (void)c;
    switch (ext) {
    case VFIO_TYPE1_IOMMU:
    case VFIO_TYPE1v2_IOMMU:
    case VFIO_UNMAP_ALL:
        return 1;
    default:
        return 0;
    }
}

/**
 * VFIO_SET_IOMMU: set the Type1 driver, version 1 or 2, giving the devices
 * of every group in `c` to a new domain. -EINVAL with no group or another
 * type, -EBUSY once set, -EPERM on a host without interrupt remapping
 * unless the boot line allows it, as Linux's allow_unsafe_interrupts,
 * -ENOMEM, or the attach's error.
 */
static int32_t set_iommu(struct vfio_container *c, uintptr_t type)
{
    struct vfio_group *g, *h;
    int r;

    if (type != VFIO_TYPE1_IOMMU && type != VFIO_TYPE1v2_IOMMU) {
        return c->groups == NULL ? -EINVAL : -ENODEV;
    }
    if (c->iommu_type != 0) {
        return -EBUSY;
    }
    if (c->groups == NULL) {
        return -EINVAL;
    }
    if (!iommu_irq_remapped() && !vfio_hw_option(UNSAFE_INTERRUPTS)) {
        klog(KLOG_WARN, "vfio",
             "set: interrupts not remapped; boot with " UNSAFE_INTERRUPTS " to allow it");
        return -EPERM;
    }
    c->domain = iommu_domain_alloc();
    if (c->domain == NULL) {
        return -ENOMEM;
    }
    for (g = c->groups; g != NULL; g = g->next_in_container) {
        r = group_attach_devices(c, g);
        if (r != 0) {
            for (h = c->groups; h != g; h = h->next_in_container) {
                group_detach_devices(c, h);
            }
            iommu_domain_free(c->domain);
            c->domain = NULL;
            return r;
        }
    }
    c->iommu_type = (int)type;
    return 0;
}

/** Take [start, end] out of the `n` sorted ranges of `out`, which may grow
 * to `max`: the ranges' new count. A range the region would split when
 * `out` is full keeps only its part below the region. */
static int exclude(struct vfio_iova_range *out, int n, int max, uint64_t start, uint64_t end)
{
    int k;

    for (k = 0; k < n; k++) {
        struct vfio_iova_range *r = &out[k];

        if (end < r->start || start > r->end) {
            continue;
        }
        if (start > r->start && end < r->end && n < max) {
            k_memmove(&out[k + 2], &out[k + 1], (size_t)(n - k - 1) * sizeof(out[0]));
            out[k + 1].start = end + 1;
            out[k + 1].end = r->end;
            r->end = start - 1;
            n++;
            k++;
        } else if (start <= r->start && end >= r->end) {
            k_memmove(&out[k], &out[k + 1], (size_t)(n - k - 1) * sizeof(out[0]));
            n--;
            k--;
        } else if (start <= r->start) {
            r->start = end + 1;
        } else {
            r->end = start - 1;
        }
    }
    return n;
}

/**
 * The IOVA ranges of `c`'s domain a mapping may use, as Linux's Type1
 * keeps them: the domain's addresses less the reserved regions of every
 * device in the container but the relaxable ones, sorted, in `out`, up
 * to `max`. Their count.
 */
static int iova_ranges(const struct vfio_container *c, struct vfio_iova_range *out, int max)
{
    struct iommu_region resv[16];
    const struct vfio_group *g;
    int n = 1, i, j, m;

    out[0].start = 0;
    out[0].end = iommu_domain_limit(c->domain) - 1;
    for (g = c->groups; g != NULL; g = g->next_in_container) {
        for (i = 0; i < g->ndev; i++) {
            const struct vfio_device *d = &vfio_devices[g->devs[i]];

            m = iommu_reserved_regions(d->pci.bus, devfn_of(d), resv, 16);
            for (j = 0; j < m; j++) {
                if (resv[j].type != IOMMU_RESV_DIRECT_RELAXABLE) {
                    n = exclude(out, n, max, resv[j].start, resv[j].end);
                }
            }
        }
    }
    return n;
}

/** Whether [iova, last] lies within one of `c`'s valid IOVA ranges. */
static int iova_valid(const struct vfio_container *c, uint64_t iova, uint64_t last)
{
    struct vfio_iova_range r[IOVA_RANGES_MAX];
    int n = iova_ranges(c, r, IOVA_RANGES_MAX), i;

    for (i = 0; i < n; i++) {
        if (iova >= r[i].start && last <= r[i].end) {
            return 1;
        }
    }
    return 0;
}

/**
 * VFIO_IOMMU_GET_INFO, as Linux's Type1 answers it: the page sizes, and
 * with room for them the capabilities, the valid IOVA ranges and the
 * mappings still available, each at an 8-byte boundary. With too little
 * room for the capabilities, `argsz` says how much they need.
 */
static int32_t get_info(struct vfio_container *c, uintptr_t arg)
{
    struct vfio_iommu_type1_info info;
    struct vfio_iommu_type1_info_cap_iova_range cap;
    struct vfio_iommu_type1_info_dma_avail avail;
    struct vfio_iova_range r[IOVA_RANGES_MAX];
    uint32_t minsz = offsetof(struct vfio_iommu_type1_info, cap_offset);
    uint32_t iova_size, avail_size, at;
    int n;

    if (copy_from_user(&info, (const void __user *)arg, minsz) != 0) {
        return -EFAULT;
    }
    if (info.argsz < minsz) {
        return -EINVAL;
    }
    if (info.argsz >= sizeof(info)) {
        minsz = sizeof(info);
    }
    info.flags = VFIO_IOMMU_INFO_PGSIZES;
    info.iova_pgsizes = PAGE_4K | 0x200000ull | 0x40000000ull;
    info.cap_offset = 0;
    info.pad = 0;
    if (minsz == sizeof(info)) {
        n = iova_ranges(c, r, IOVA_RANGES_MAX);
        iova_size = (uint32_t)(sizeof(cap) + (uint32_t)n * sizeof(r[0]));
        avail_size = (uint32_t)((sizeof(avail) + 7u) & ~7u);
        info.flags |= VFIO_IOMMU_INFO_CAPS;
        if (info.argsz < sizeof(info) + iova_size + avail_size) {
            info.argsz = (uint32_t)(sizeof(info) + iova_size + avail_size);
        } else {
            at = (uint32_t)sizeof(info);
            k_memset(&cap, 0, sizeof(cap));
            cap.header.id = VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE;
            cap.header.version = 1;
            cap.header.next = at + iova_size;
            cap.nr_iovas = (uint32_t)n;
            k_memset(&avail, 0, sizeof(avail));
            avail.header.id = VFIO_IOMMU_TYPE1_INFO_DMA_AVAIL;
            avail.header.version = 1;
            avail.avail = VFIO_DMA_MAX - c->ndma;
            if (copy_to_user((void __user *)(arg + at), &cap, sizeof(cap)) != 0 ||
                copy_to_user((void __user *)(arg + at + sizeof(cap)), r,
                             (uint32_t)n * sizeof(r[0])) != 0 ||
                copy_to_user((void __user *)(arg + at + iova_size), &avail, sizeof(avail)) != 0) {
                return -EFAULT;
            }
            info.cap_offset = at;
        }
    }
    return copy_to_user((void __user *)arg, &info, minsz) != 0 ? -EFAULT : 0;
}

/** What a mapping does when an unmap, or for device pages the end of the
 * address space, takes its pages back: the device no longer reaches them,
 * as the pin requires. A mapping of RAM outlives the end of the address
 * space (PIN_KEEP_PAST_END) until the container's release, after the
 * device's own release at the close of its descriptor has reset it. */
static void dma_released(struct pin *pin, void *owner)
{
    struct vfio_dma *dma = (struct vfio_dma *)owner;

    if (dma->container->domain != NULL) {
        klog(KLOG_INFO, "vfio", "dma: taken back: 0x%lx, %lu pages, %lu of them RAM",
             (unsigned long)dma->iova, (unsigned long)pin_count(pin),
             (unsigned long)pin_ram_count(pin));
        (void)iommu_unmap(dma->container->domain, dma->iova, dma->size);
    }
}

/**
 * Log, as `vfio: release: NAME: ...`, how many pages of `c`'s live
 * mappings of RAM its domain no longer maps to their pinned frames, the
 * first of them, and the DMA faults counted so far. A device of `c` that
 * still runs has had its reads and writes of such a page blocked since it
 * went. Called at the release of the device NAME, before its reset, with
 * the big kernel lock held; reads the domain's tables page by page and
 * takes no lock, since nothing maps or unmaps in `c` meanwhile.
 */
void vfio_container_audit(const struct vfio_container *c, const char *name)
{
    const struct vfio_dma *dma;
    unsigned long pages = 0, lost = 0;
    uint64_t first = 0, iova;
    uint32_t i, n;

    if (c == NULL || c->domain == NULL) {
        return;
    }
    for (dma = c->dmas; dma != NULL; dma = dma->next) {
        if (dma->pin == NULL || !pin_live(dma->pin) ||
            pin_ram_count(dma->pin) != pin_count(dma->pin)) {
            continue;
        }
        n = pin_count(dma->pin);
        for (i = 0; i < n; i++) {
            iova = dma->iova + (uint64_t)i * PAGE_4K;
            if (iommu_iova_to_phys(c->domain, iova) != (uint64_t)pin_phys(dma->pin, i) &&
                lost++ == 0) {
                first = iova;
            }
        }
        pages += n;
    }
    klog(lost != 0 ? KLOG_WARN : KLOG_INFO, "vfio",
         "release: %s: %lu of %lu pages of RAM out of the domain, the first at 0x%lx; %lu DMA "
         "faults",
         name, lost, pages, (unsigned long)first, iommu_dma_faults());
}

/** Map the pinned pages of `dma` at its IOVA, each run of physically
 * contiguous pages at once: 0, or the error with nothing mapped. */
static int dma_map_pages(struct vfio_container *c, struct vfio_dma *dma)
{
    uint32_t pages = pin_count(dma->pin), i = 0, run;
    uint64_t phys;
    int r;

    while (i < pages) {
        phys = pin_phys(dma->pin, i);
        for (run = 1; i + run < pages && pin_phys(dma->pin, i + run) == phys + run * PAGE_4K;
             run++) {
        }
        r = iommu_map(c->domain, dma->iova + (uint64_t)i * PAGE_4K, phys, run * PAGE_4K, dma->prot);
        if (r != 0) {
            if (i != 0) {
                (void)iommu_unmap(c->domain, dma->iova, (uint64_t)i * PAGE_4K);
            }
            return r;
        }
        i += run;
    }
    return 0;
}

/** The checks of a MAP_DMA that need no pages, in Linux's order. 0 or the
 * errno. */
static int32_t map_check(const struct vfio_container *c, const struct vfio_iommu_type1_dma_map *m)
{
    const struct vfio_dma *dma;
    uint64_t last = m->iova + m->size - 1;

    if ((m->flags & ~(VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE)) != 0) {
        return -EINVAL;
    }
    if ((m->flags & (VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE)) == 0 || m->size == 0 ||
        ((m->size | m->iova | m->vaddr) & (PAGE_4K - 1)) != 0) {
        return -EINVAL;
    }
    if (last < m->iova || m->vaddr + m->size - 1 < m->vaddr ||
        m->vaddr + m->size - 1 > (uint64_t)(uintptr_t)-1) {
        return -EINVAL;
    }
    for (dma = c->dmas; dma != NULL; dma = dma->next) {
        if (dma->iova <= last && m->iova <= dma->iova + dma->size - 1) {
            return -EEXIST;
        }
    }
    if (c->ndma >= VFIO_DMA_MAX) {
        return -ENOSPC;
    }
    if (!iova_valid(c, m->iova, last)) {
        return -EINVAL;
    }
    return 0;
}

/** VFIO_IOMMU_MAP_DMA: pin the caller's [vaddr, vaddr + size) and map it at
 * [iova, iova + size) with the access the flags give. The pin may sleep,
 * and another thread may meanwhile map the same IOVA or take the last
 * group out, which releases the driver, so the checks run again after it,
 * as Linux's Type1 makes them all under iommu->lock. */
static int32_t map_dma(struct vfio_container *c, uintptr_t arg)
{
    struct vfio_iommu_type1_dma_map m;
    struct vfio_dma *dma, **pp;
    int32_t r;

    if (copy_from_user(&m, (const void __user *)arg, sizeof(m)) != 0) {
        return -EFAULT;
    }
    if (m.argsz < sizeof(m)) {
        return -EINVAL;
    }
    r = map_check(c, &m);
    if (r != 0) {
        return r;
    }
    dma = (struct vfio_dma *)kzalloc(sizeof(*dma));
    if (dma == NULL) {
        return -ENOMEM;
    }
    dma->iova = m.iova;
    dma->size = m.size;
    dma->vaddr = m.vaddr;
    dma->prot = ((m.flags & VFIO_DMA_MAP_FLAG_READ) ? IOMMU_READ : 0) |
                ((m.flags & VFIO_DMA_MAP_FLAG_WRITE) ? IOMMU_WRITE : 0);
    dma->container = c;
    dma->holder.release = dma_released;
    dma->holder.owner = dma;
    dma->holder.flags = PIN_KEEP_PAST_END;
    r = pin_user(vfio_hw_mm(), (uintptr_t)m.vaddr, (uintptr_t)m.size,
                 PIN_IO | PIN_SLOW | PIN_LONGTERM |
                     ((m.flags & VFIO_DMA_MAP_FLAG_WRITE) ? PIN_WRITE : 0u),
                 &dma->holder, &dma->pin);
    if (r == 0) {
        r = c->iommu_type == 0 ? -EINVAL : map_check(c, &m);
        if (r == 0) {
            r = dma_map_pages(c, dma);
        }
        if (r != 0) {
            unpin(dma->pin);
        }
    }
    if (r != 0) {
        kfree(dma);
        vm_file_reap();
        return r;
    }
    for (pp = &c->dmas; *pp != NULL && (*pp)->iova < dma->iova; pp = &(*pp)->next) {
    }
    dma->next = *pp;
    *pp = dma;
    c->ndma++;
    return 0;
}

/**
 * VFIO_IOMMU_UNMAP_DMA: remove the mappings in [iova, iova + size), or with
 * VFIO_DMA_UNMAP_FLAG_ALL every one, and write the bytes removed in `size`.
 * Version 2 refuses with -EINVAL a range that would split a mapping;
 * version 1 removes each mapping the range touches whole, as Linux's.
 */
static int32_t unmap_dma(struct vfio_container *c, uintptr_t arg)
{
    struct vfio_iommu_type1_dma_unmap u;
    struct vfio_dma *dma, *next;
    uint64_t last, done = 0;
    int all;

    if (copy_from_user(&u, (const void __user *)arg, sizeof(u)) != 0) {
        return -EFAULT;
    }
    if (u.argsz < sizeof(u) || (u.flags & ~VFIO_DMA_UNMAP_FLAG_ALL) != 0) {
        return -EINVAL;
    }
    all = (u.flags & VFIO_DMA_UNMAP_FLAG_ALL) != 0;
    if (all ? (u.iova != 0 || u.size != 0)
            : (u.size == 0 || ((u.iova | u.size) & (PAGE_4K - 1)) != 0 ||
               u.iova + u.size - 1 < u.iova)) {
        return -EINVAL;
    }
    last = all ? ~0ull : u.iova + u.size - 1;
    if (c->iommu_type == VFIO_TYPE1v2_IOMMU && !all) {
        for (dma = c->dmas; dma != NULL; dma = dma->next) {
            uint64_t dlast = dma->iova + dma->size - 1;

            if ((dma->iova < u.iova && dlast >= u.iova) || (dma->iova <= last && dlast > last)) {
                return -EINVAL;
            }
        }
    }
    for (dma = c->dmas; dma != NULL; dma = next) {
        next = dma->next;
        if (dma->iova <= last && dma->iova + dma->size - 1 >= (all ? 0 : u.iova)) {
            done += dma->size;
            dma_remove(c, dma);
        }
    }
    vm_file_reap();
    u.size = done;
    return copy_to_user((void __user *)arg, &u, sizeof(u)) != 0 ? -EFAULT : 0;
}

/** The ioctls of a container. Those of the driver are -EINVAL before it is
 * set, as Linux's container answers with no driver. */
static int32_t container_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct vfio_container *c = (struct vfio_container *)e->priv;

    switch (request) {
    case VFIO_GET_API_VERSION:
        return VFIO_API_VERSION;
    case VFIO_CHECK_EXTENSION:
        return vfio_container_check_extension(c, arg);
    case VFIO_SET_IOMMU:
        return set_iommu(c, arg);
    default:
        break;
    }
    if (c->iommu_type == 0) {
        return -EINVAL;
    }
    switch (request) {
    case VFIO_IOMMU_GET_INFO:
        return get_info(c, arg);
    case VFIO_IOMMU_MAP_DMA:
        return map_dma(c, arg);
    case VFIO_IOMMU_UNMAP_DMA:
        return unmap_dma(c, arg);
    default:
        return -ENOTTY;
    }
}
