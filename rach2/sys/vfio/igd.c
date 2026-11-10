/*
 * igd.c: the regions of an Intel graphics device that Linux's
 * vfio_pci_igd.c gives its owner: the OpRegion the firmware left for the
 * graphics driver, with its extended VBT after it, and read-only views of
 * the host bridge's and the LPC bridge's configuration spaces.
 */

#include "vfio.h"

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

#define OPREGION_SIZE     0x2000u /* the OpRegion's mapped bytes, 8 KiB */
#define OPREGION_PCI_ADDR 0xFC    /* ASLS: its physical address */
#define OPREGION_SIZE_KB  0x10    /* its size field, in KiB */
#define OPREGION_VERSION  0x16
#define OPREGION_RVDA     0x3BA /* the extended VBT's address */
#define OPREGION_RVDS     0x3C2 /* and its size */

#define PCI_CLASS_BRIDGE_HOST 0x0600u
#define PCI_CLASS_BRIDGE_ISA  0x0601u

/** The bytes at `p`, little-endian, `n` of them up to 8. */
static uint64_t le(const volatile uint8_t *p, unsigned int n)
{
    uint64_t v = 0;

    while (n-- > 0) {
        v = v << 8 | p[n];
    }
    return v;
}

/** Whether `base` begins with the OpRegion's signature. */
static int signed_opregion(const volatile uint8_t *base)
{
    static const char sig[] = "IntelGraphicsMem";
    unsigned int i;

    for (i = 0; i < 16; i++) {
        if (base[i] != (uint8_t)sig[i]) {
            return 0;
        }
    }
    return 1;
}

/** Whether the function at 00:`dev`.0 is Intel's and of the class `cls`,
 * base and sub-class. */
static int bridge_is(uint8_t dev, uint16_t cls)
{
    return pci_read_config16(0, dev, 0, PCI_VENDOR_ID) == 0x8086 &&
           (pci_read_config32(0, dev, 0, 0x08) >> 16) == cls;
}

/**
 * Map the OpRegion whose address ASLS holds and, from version 2.0, the
 * extended VBT that RVDA and RVDS name, an absolute address in 2.0 and one
 * relative to the OpRegion after, as Linux's vfio_pci_igd_opregion_init.
 * Then add the bridges' regions when both are Intel's. 0; -ENODEV when
 * ASLS holds no address; -EINVAL for a region without the signature or a
 * size; -ENOMEM when it cannot be mapped.
 */
int vfio_igd_init(struct vfio_device *d)
{
    struct vfio_igd *g = &d->igd;
    uint32_t addr =
        pci_read_config32(d->pci.bus, d->pci.device, d->pci.function, OPREGION_PCI_ADDR);
    const volatile uint8_t *base;
    uint64_t rvda, vbt;
    uint32_t rvds;

    k_memset(g, 0, sizeof(*g));
    if (addr == 0 || addr == 0xFFFFFFFFu) {
        return -ENODEV;
    }
    g->va = vfio_hw_map(addr, OPREGION_SIZE);
    if (g->va == 0) {
        return -ENOMEM;
    }
    base = (const volatile uint8_t *)g->va;
    if (!signed_opregion(base) || le(base + OPREGION_SIZE_KB, 4) == 0) {
        vfio_igd_free(d);
        return -EINVAL;
    }
    g->version = (uint16_t)le(base + OPREGION_VERSION, 2);
    g->size = OPREGION_SIZE;
    rvda = g->version >= 0x0200 ? le(base + OPREGION_RVDA, 8) : 0;
    rvds = g->version >= 0x0200 ? (uint32_t)le(base + OPREGION_RVDS, 4) : 0;
    if (rvda != 0 && rvds != 0) {
        vbt = g->version == 0x0200 ? rvda : addr + rvda;
        g->vbt_va = vfio_hw_map(vbt, rvds);
        if (g->vbt_va == 0) {
            vfio_igd_free(d);
            return -ENOMEM;
        }
        g->vbt_size = rvds;
        g->size += rvds;
        if (g->version == 0x0200) {
            g->version = 0x0201; /* RVDA is now relative, as 2.1 has it */
        }
    }
    g->present = 1;
    g->cfg = bridge_is(0x00, PCI_CLASS_BRIDGE_HOST) && bridge_is(0x1F, PCI_CLASS_BRIDGE_ISA);
    klog(KLOG_INFO, "vfio", "igd: %s: OpRegion %x.%02x, %u bytes%s", d->name, g->version >> 8,
         g->version & 0xFFu, (unsigned int)g->size, g->cfg ? ", bridges" : "");
    return 0;
}

void vfio_igd_free(struct vfio_device *d)
{
    struct vfio_igd *g = &d->igd;

    if (g->vbt_va != 0) {
        vfio_hw_unmap(g->vbt_va, g->vbt_size);
    }
    if (g->va != 0) {
        vfio_hw_unmap(g->va, OPREGION_SIZE);
    }
    k_memset(g, 0, sizeof(*g));
}

/** The regions the IGD adds after VFIO_PCI_NUM_REGIONS. */
uint32_t vfio_igd_regions(const struct vfio_device *d)
{
    if (!d->igd.present) {
        return 0;
    }
    return d->igd.cfg ? 3u : 1u;
}

/** The bytes of the IGD's region `index`. */
uint32_t vfio_igd_region_size(const struct vfio_device *d, uint32_t index)
{
    if (index == VFIO_IGD_OPREGION_INDEX) {
        return d->igd.size;
    }
    return pci_read_config32_ext(0, index == VFIO_IGD_HOST_INDEX ? 0 : 0x1F, 0, 0x100) !=
                   0xFFFFFFFFu
               ? 4096u
               : 256u;
}

/** Byte `off` of the OpRegion region: the OpRegion with its version and
 * RVDA as the owner reads them, then the extended VBT. */
static uint8_t opregion_byte(const struct vfio_igd *g, uint32_t off)
{
    const volatile uint8_t *base = (const volatile uint8_t *)g->va;
    uint64_t rvda = g->vbt_va != 0 ? OPREGION_SIZE : 0;

    if (off >= OPREGION_SIZE) {
        return ((const volatile uint8_t *)g->vbt_va)[off - OPREGION_SIZE];
    }
    if (off >= OPREGION_VERSION && off < OPREGION_VERSION + 2) {
        return (uint8_t)(g->version >> (8 * (off - OPREGION_VERSION)));
    }
    if (off >= OPREGION_RVDA && off < OPREGION_RVDA + 8) {
        return (uint8_t)(rvda >> (8 * (off - OPREGION_RVDA)));
    }
    return base[off];
}

/** Read [pos, pos + count) of a bridge's configuration space, `dev`, a
 * byte at a time from the aligned dwords, which reading has no effect on. */
static void bridge_read(uint8_t dev, uint32_t pos, uint8_t *buf, uint32_t count)
{
    uint32_t i, dw;

    for (i = 0; i < count; i++) {
        uint32_t at = pos + i;

        dw = at >= 256 ? pci_read_config32_ext(0, dev, 0, (uint16_t)(at & ~3u))
                       : pci_read_config32(0, dev, 0, (uint8_t)(at & ~3u));
        buf[i] = (uint8_t)(dw >> (8 * (at & 3u)));
    }
}

/**
 * Read `count` bytes from `pos` of the IGD's region `index` into the kernel
 * buffer `buf`: the bytes read, cut at the region's end, or -EINVAL for a
 * position past it or a region the device does not have.
 */
int32_t vfio_igd_read(struct vfio_device *d, uint32_t index, uint32_t pos, uint8_t *buf,
                      uint32_t count)
{
    uint32_t size, i;

    if (index < VFIO_IGD_OPREGION_INDEX || index >= VFIO_IGD_OPREGION_INDEX + vfio_igd_regions(d)) {
        return -EINVAL;
    }
    size = vfio_igd_region_size(d, index);
    if (pos >= size) {
        return -EINVAL;
    }
    if (count > size - pos) {
        count = size - pos;
    }
    if (index == VFIO_IGD_OPREGION_INDEX) {
        for (i = 0; i < count; i++) {
            buf[i] = opregion_byte(&d->igd, pos + i);
        }
    } else {
        bridge_read(index == VFIO_IGD_HOST_INDEX ? 0 : 0x1F, pos, buf, count);
    }
    return (int32_t)count;
}
