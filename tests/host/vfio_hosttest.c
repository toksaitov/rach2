/*
 * vfio_hosttest.c: VFIO, rach2/sys/vfio/, against models of the PCI bus,
 * the IOMMU, pins, eventfds, vectors, /sys and /dev.
 *
 * host-deps: rach2/sys/vfio/group.c rach2/sys/vfio/container.c
 * host-deps: rach2/sys/vfio/pci.c rach2/sys/vfio/config.c
 * host-deps: rach2/sys/vfio/irq.c rach2/sys/vfio/igd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/atomic_host.c tests/host/compat/fiber_host.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * The bus is the Lenovo's Haswell in small: the host bridge, the HD 4400
 * with its OpRegion, an xHCI with MSI-X, a Lynx Point root port that the
 * Intel PCH quirk isolates with a network card behind it, a root port
 * without ACS with an endpoint behind it, and the LPC bridge with two
 * functions beside it, one held by a kernel driver. The rows check the
 * groups as Linux's pci_device_group forms them and their entries of /sys
 * and /dev; binding by driver_override, new_id and drivers_probe, which
 * offers virtio-pci a function vfio-pci does not match; the group and
 * container ioctls; Type1's checks, mappings and capabilities;
 * the device's regions and their information; the configuration space as
 * the owner sees it, the power state the owner writes among it; the line,
 * MSI and MSI-X into eventfds; and the reset at the open, at
 * VFIO_DEVICE_RESET and at the release, which puts the device in D0 first,
 * with every vector and eventfd given back.
 */

#include "vfio/vfio.h"

#include "fs/devfs.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "fs/synthfs.h"
#include "fs/sysfs.h"
#include "io/eventfd.h"
#include "iommu.h"
#include "irq.h"
#include "lib/kerrno.h"
#include "lib/kstring.h"
#include "mm/kheap.h"
#include "mm/paging.h"
#include "mm/pin.h"
#include "mm/usercopy.h"
#include "mm/vm.h"
#include "proc/ns.h"
#include "proc/percpu.h"
#include "proc/process.h"
#include "compat/fiber_host.h"

extern int printf(const char *format, ...);

/* The writer of /sys: root in the first user namespace, less the
 * capabilities a row takes away, by number below 32. */
static process_t writer;
struct percpu cpu_table[NR_CPUS] = {{.current = &writer}};
static uint32_t writer_caps_lost;

int cap_holds(const process_t *p, uint32_t cap)
{
    return p->euid == 0 && (cap >= 32u || !(writer_caps_lost & (1u << cap)));
}
extern void *malloc(unsigned long n);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);
extern int strcmp(const char *a, const char *b);
extern int memcmp(const void *a, const void *b, unsigned long n);

static int failures;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                           \
    do {                                                   \
        if (!(condition)) {                                \
            printf("vfio-hosttest: failed: %s\n", detail); \
            failures++;                                    \
        }                                                  \
    } while (0)

/* ---- the bus ---- */

#define NFN 12

/* A function of the model: its place, configuration space, BAR and ROM
 * sizes, owner, reset method and the resets it took. */
struct fn {
    uint8_t bus, dev, fn;
    uint8_t cfg[4096];
    uint64_t barsz[6];
    uint32_t romsz;
    const char *owner;
    int method;
    int resets;
    int unit;             /* its IOMMU unit, or -1 */
    uint8_t mem[0x10000]; /* BAR 0's first 64 KiB */
};

static struct fn fns[NFN];
static int nfns;

/** The function at (bus, dev, fn), or NULL. */
static struct fn *fn_at(uint8_t bus, uint8_t dev, uint8_t f)
{
    int i;

    for (i = 0; i < nfns; i++) {
        if (fns[i].bus == bus && fns[i].dev == dev && fns[i].fn == f) {
            return &fns[i];
        }
    }
    return NULL;
}

/** Store `v` at `p`, little-endian, as configuration space holds it. */
static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/** Store the dword `v` at `p`, little-endian. */
static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

/** The dword at `p`, little-endian. */
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Add a function with its identity, device << 16 | vendor, its class and
 * its header type. */
static struct fn *add_fn(uint8_t bus, uint8_t dev, uint8_t f, uint32_t id, uint16_t cls,
                         uint8_t header)
{
    struct fn *m = &fns[nfns++];

    k_memset(m, 0, sizeof(*m));
    m->bus = bus;
    m->dev = dev;
    m->fn = f;
    m->unit = 1;
    put32(m->cfg, id);
    put16(m->cfg + 0x0A, cls);
    m->cfg[0x0E] = header;
    return m;
}

/** Link a capability `id` at `at` of `len` bytes into the list. */
static void add_cap(struct fn *m, uint8_t id, uint8_t at)
{
    uint8_t *p = &m->cfg[0x34];

    m->cfg[6] |= 0x10;
    while (*p != 0) {
        p = &m->cfg[*p + 1];
    }
    *p = at;
    m->cfg[at] = id;
    m->cfg[at + 1] = 0;
}

/** Give BAR `i` the address `addr` and size `size`, 64-bit when `wide`. */
static void add_bar(struct fn *m, int i, uint64_t addr, uint64_t size, int io, int wide)
{
    m->barsz[i] = size;
    put32(m->cfg + 0x10 + 4 * i, (uint32_t)addr | (io ? 1u : wide ? 4u : 0u));
    if (wide) {
        put32(m->cfg + 0x14 + 4 * i, (uint32_t)(addr >> 32));
    }
}

uint32_t pci_read_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    struct fn *m = fn_at(bus, device, function);

    return m != NULL ? get32(m->cfg + (offset & 0xFC)) : 0xFFFFFFFFu;
}

uint32_t pci_read_config32_ext(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset)
{
    struct fn *m = fn_at(bus, device, function);

    if (m == NULL || (offset >= 256 && get32(m->cfg + 0x100) == 0)) {
        return 0xFFFFFFFFu;
    }
    return get32(m->cfg + (offset & 0xFFC));
}

uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return (uint16_t)(pci_read_config32(bus, device, function, offset) >> (8 * (offset & 2)));
}

uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    return (uint8_t)(pci_read_config32(bus, device, function, offset) >> (8 * (offset & 3)));
}

/** A dword write as a device takes it: a BAR or the ROM keeps only the bits
 * its size lets it hold. */
void pci_write_config32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint32_t value)
{
    struct fn *m = fn_at(bus, device, function);
    int i = (offset - 0x10) / 4;

    if (m == NULL) {
        return;
    }
    offset &= 0xFC;
    if (offset >= 0x10 && offset < 0x28 && m->barsz[i] != 0) {
        uint32_t flags = get32(m->cfg + offset) & ((m->cfg[offset] & 1) ? 3u : 0xFu);

        value = (value & ~(uint32_t)(m->barsz[i] - 1) & ~0xFu) | flags;
    } else if (offset == 0x30) {
        value = m->romsz != 0 ? (value & ~(m->romsz - 1) & 0xFFFFF800u) | (value & 1u) : 0;
    }
    put32(m->cfg + offset, value);
}

void pci_write_config32_ext(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset,
                            uint32_t value)
{
    struct fn *m = fn_at(bus, device, function);

    if (m != NULL) {
        put32(m->cfg + (offset & 0xFFC), value);
    }
}

void pci_write_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset,
                        uint16_t value)
{
    struct fn *m = fn_at(bus, device, function);

    if (m != NULL) {
        put16(m->cfg + (offset & 0xFE), value);
    }
}

void pci_write_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value)
{
    struct fn *m = fn_at(bus, device, function);

    if (m != NULL) {
        m->cfg[offset] = value;
    }
}

uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t cap_id)
{
    struct fn *m = fn_at(bus, device, function);
    uint8_t at;

    if (m == NULL || !(m->cfg[6] & 0x10)) {
        return 0;
    }
    for (at = m->cfg[0x34]; at != 0; at = m->cfg[at + 1]) {
        if (m->cfg[at] == cap_id) {
            return at;
        }
    }
    return 0;
}

/** Fill `out` from the model's function `m`. */
static void fill(const struct fn *m, pci_device_t *out)
{
    int i;

    k_memset(out, 0, sizeof(*out));
    for (i = 0; i < 6; i++) {
        out->bar[i] = get32(m->cfg + 0x10 + 4 * i);
    }
    out->vendor_id = (uint16_t)(m->cfg[0] | m->cfg[1] << 8);
    out->device_id = (uint16_t)(m->cfg[2] | m->cfg[3] << 8);
    out->class_code = m->cfg[0x0B];
    out->subclass = m->cfg[0x0A];
    out->bus = m->bus;
    out->device = m->dev;
    out->function = m->fn;
    out->irq = m->cfg[0x3C];
}

bool pci_nth(int nth, pci_device_t *out)
{
    if (nth < 0 || nth >= nfns) {
        return false;
    }
    fill(&fns[nth], out);
    return true;
}

bool pci_find_bridge_for_bus(uint8_t bus, pci_device_t *out)
{
    int i;

    for (i = 0; i < nfns; i++) {
        if ((fns[i].cfg[0x0E] & 0x7F) == 1 && fns[i].cfg[0x19] == bus) {
            fill(&fns[i], out);
            return true;
        }
    }
    return false;
}

/** A memory BAR's size; 0 for an I/O BAR, as the kernel's sizes none. */
uint64_t pci_bar_size64(const pci_device_t *dev, int i)
{
    const struct fn *m = fn_at(dev->bus, dev->device, dev->function);

    return (m->cfg[0x10 + 4 * i] & 1) ? 0 : m->barsz[i];
}

/** BAR `i` as the bus's walk recorded it: its address now and its size. */
void pci_resource(uint8_t bus, uint8_t device, uint8_t function, int i, struct pci_resource *r)
{
    const struct fn *m = fn_at(bus, device, function);
    uint32_t v;

    k_memset(r, 0, sizeof(*r));
    if (m == NULL || i < 0 || i > 5 || m->barsz[i] == 0) {
        return;
    }
    v = get32(m->cfg + 0x10 + 4 * i);
    r->start = v & ((v & 1u) ? ~3u : ~0xFu);
    r->end = r->start + m->barsz[i] - 1u;
    r->flags = ((v & 1u) ? (v & 3u) | PCI_IORESOURCE_IO : (v & 0xFu) | PCI_IORESOURCE_MEM) |
               PCI_IORESOURCE_SIZEALIGN;
}

/** A BAR is claimed when the walk sized it and it has an address: this
 * bus's BARs lie on no fixed port and over no other. */
int pci_resource_claimable(uint8_t bus, uint8_t device, uint8_t function, int i)
{
    struct pci_resource r;

    pci_resource(bus, device, function, i, &r);
    return r.flags != 0 && r.start != 0 ? 0 : -EINVAL;
}

/** The function's power state, or -1 with no power management. */
int pci_power_state(const pci_device_t *dev)
{
    const struct fn *m = fn_at(dev->bus, dev->device, dev->function);
    uint8_t pm = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PM);

    return pm != 0 ? m->cfg[pm + 4] & 3 : -1;
}

/* Nonzero while the function refuses every change of its power state. */
static int power_refused;

/** A change of power state as a function takes it: from D3hot to D0 with
 * No_Soft_Reset clear, its command word and BARs come back cleared. While
 * `power_refused` is set it keeps the state it is in. */
int pci_set_power_state(const pci_device_t *dev, int state)
{
    struct fn *m = fn_at(dev->bus, dev->device, dev->function);
    uint8_t pm = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PM);
    int i, was;

    if (pm == 0) {
        return -1;
    }
    if (power_refused) {
        return 0;
    }
    was = m->cfg[pm + 4] & 3;
    m->cfg[pm + 4] = (uint8_t)((m->cfg[pm + 4] & ~3) | (state & 3));
    if (was == PCI_D3HOT && state == PCI_D0 && !(m->cfg[pm + 4] & 0x08)) {
        put16(m->cfg + 4, 0);
        for (i = 0; i < 6; i++) {
            m->cfg[0x10 + 4 * i] &= (uint8_t)((m->cfg[0x10 + 4 * i] & 1) ? 3 : 0xF);
            k_memset(m->cfg + 0x11 + 4 * i, 0, 3);
        }
        if (pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSI)) {
            pci_msi_disable(dev);
        }
        if (pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSIX)) {
            pci_msix_disable(dev);
            k_memset(m->mem + 0x2000, 0, 8 * 16);
        }
    }
    return 0;
}

uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    const uint8_t *c = fn_at(dev->bus, dev->device, dev->function)->cfg;
    uint64_t lo = get32(c + 0x10 + 4 * i) & ~0xFu;

    return (get32(c + 0x10 + 4 * i) & 6u) == 4u ? lo | (uint64_t)get32(c + 0x14 + 4 * i) << 32 : lo;
}

static int windows;  /* windows mapped now */
static int bar_maps; /* windows asked for, made or not */

uintptr_t pci_bar_map(const pci_device_t *dev, int i, uint64_t offset, size_t size, pte_t cache,
                      struct pci_window *w)
{
    struct fn *m = fn_at(dev->bus, dev->device, dev->function);

    (void)cache;
    bar_maps++;
    if (i != 0 || offset + size > sizeof(m->mem)) {
        return 0;
    }
    w->va = (uintptr_t)(m->mem + offset);
    w->size = size;
    windows++;
    return w->va;
}

void pci_bar_unmap(struct pci_window *w)
{
    if (w->va != 0) {
        windows--;
    }
    w->va = 0;
}

static int msi_vector = -1; /* the vector MSI was last enabled with */
static int msi_enables;
static int msix_vectors[16]; /* each MSI-X entry's vector, or -1 */
static int msix_unmasks;

int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)dev;
    msi_vector = vector;
    msi_enables++;
    return 0;
}

void pci_msi_disable(const pci_device_t *dev)
{
    (void)dev;
    msi_vector = -1;
}

int pci_msix_table(const pci_device_t *dev, uint8_t *bar, uint32_t *offset, uint32_t *entries)
{
    struct fn *m = fn_at(dev->bus, dev->device, dev->function);
    uint8_t at = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSIX);

    if (at == 0) {
        return -1;
    }
    *bar = (uint8_t)(get32(m->cfg + at + 4) & 7u);
    *offset = get32(m->cfg + at + 4) & ~7u;
    *entries = ((uint32_t)(m->cfg[at + 2] | m->cfg[at + 3] << 8) & 0x7FFu) + 1u;
    return 0;
}

int pci_msix_hold(const pci_device_t *dev)
{
    (void)dev;
    return 0;
}

void pci_msix_disable(const pci_device_t *dev)
{
    int i;

    (void)dev;
    for (i = 0; i < 16; i++) {
        msix_vectors[i] = -1;
    }
}

int pci_msix_entry_set(const pci_device_t *dev, uintptr_t table, unsigned int index, uint8_t vector)
{
    struct fn *m = fn_at(dev->bus, dev->device, dev->function);

    if (table != (uintptr_t)(m->mem + 0x2000) || index >= 16) {
        return -1;
    }
    msix_vectors[index] = vector;
    put32(m->mem + 0x2000 + index * 16 + 12, 0);
    return 0;
}

void pci_msix_entry_clear(const pci_device_t *dev, unsigned int index)
{
    (void)dev;
    msix_vectors[index] = -1;
}

void pci_msix_unmask(const pci_device_t *dev)
{
    (void)dev;
    msix_unmasks++;
}

static uint8_t ports[0x10000];

uint8_t pci_io_read8(uint16_t port)
{
    return ports[port];
}

uint16_t pci_io_read16(uint16_t port)
{
    return (uint16_t)(ports[port] | ports[port + 1] << 8);
}

uint32_t pci_io_read32(uint16_t port)
{
    return get32(&ports[port]);
}

void pci_io_write8(uint16_t port, uint8_t value)
{
    ports[port] = value;
}

void pci_io_write16(uint16_t port, uint16_t value)
{
    put16(&ports[port], value);
}

void pci_io_write32(uint16_t port, uint32_t value)
{
    put32(&ports[port], value);
}

int pci_claim(uint8_t bus, uint8_t devfn, const char *owner)
{
    struct fn *m = fn_at(bus, devfn >> 3, devfn & 7);

    if (m->owner != NULL) {
        return -EBUSY;
    }
    m->owner = owner;
    return 0;
}

void pci_unclaim(uint8_t bus, uint8_t devfn, const char *owner)
{
    struct fn *m = fn_at(bus, devfn >> 3, devfn & 7);

    if (m->owner != NULL && strcmp(m->owner, owner) == 0) {
        m->owner = NULL;
    }
}

const char *pci_owner(uint8_t bus, uint8_t devfn)
{
    return fn_at(bus, devfn >> 3, devfn & 7)->owner;
}

/* The functions drivers_probe offered virtio-pci: how many, and the last
 * one's bus << 8 | devfn. virtio_pci_shm_hosttest.c tests the bind. */
static int virtio_offers, virtio_offered = -1;

int virtio_pci_bind(uint8_t bus, uint8_t devfn)
{
    virtio_offers++;
    virtio_offered = bus << 8 | devfn;
    return -ENODEV;
}

int pci_reset_method(const pci_device_t *dev)
{
    return fn_at(dev->bus, dev->device, dev->function)->method;
}

void pci_save_state(const pci_device_t *dev, struct pci_saved_state *st)
{
    k_memset(st, 0, sizeof(*st));
    k_memcpy(st->header, fn_at(dev->bus, dev->device, dev->function)->cfg, 64);
}

void pci_restore_state(const pci_device_t *dev, const struct pci_saved_state *st)
{
    k_memcpy(fn_at(dev->bus, dev->device, dev->function)->cfg, st->header, 64);
}

/** A reset as pci_reset_function makes one: the state kept, the function
 * reset, which here clears its command word, and the state put back. */
int pci_reset_function(const pci_device_t *dev, pci_sleep_t msleep)
{
    struct fn *m = fn_at(dev->bus, dev->device, dev->function);
    struct pci_saved_state st;

    if (m->method == PCI_RESET_NONE) {
        return -ENOTTY;
    }
    pci_save_state(dev, &st);
    put16(m->cfg + 4, 0);
    msleep(100);
    pci_restore_state(dev, &st);
    m->resets++;
    return 0;
}

/* ---- the IOMMU ---- */

static int remapped;
static struct {
    uint64_t iova, phys, size;
    int prot;
} maps[64];
static int nmaps, unmaps, attaches, detaches, domains;
static uint64_t unmapped;

struct iommu_domain {
    int id;
};
static struct iommu_domain the_domain;

int iommu_present(void)
{
    return 1;
}

int iommu_irq_remapped(void)
{
    return remapped;
}

int iommu_unit_of(uint8_t bus, uint8_t devfn)
{
    struct fn *m = fn_at(bus, devfn >> 3, devfn & 7);

    return m != NULL ? m->unit : -1;
}

struct iommu_domain *iommu_domain_alloc(void)
{
    domains++;
    return &the_domain;
}

void iommu_domain_free(struct iommu_domain *d)
{
    (void)d;
    domains--;
}

uint64_t iommu_domain_limit(const struct iommu_domain *d)
{
    (void)d;
    return 1ull << 39;
}

int iommu_map(struct iommu_domain *d, uint64_t iova, uint64_t phys, uint64_t size, int prot)
{
    (void)d;
    if (nmaps < 64) {
        maps[nmaps].iova = iova;
        maps[nmaps].phys = phys;
        maps[nmaps].size = size;
        maps[nmaps].prot = prot;
        nmaps++;
    }
    return 0;
}

int iommu_unmap(struct iommu_domain *d, uint64_t iova, uint64_t size)
{
    (void)d;
    (void)iova;
    unmaps++;
    unmapped += size;
    return 0;
}

uint64_t iommu_iova_to_phys(const struct iommu_domain *d, uint64_t iova)
{
    (void)d;
    (void)iova;
    return ~0ull;
}

unsigned long iommu_dma_faults(void)
{
    return 0;
}

int iommu_attach(struct iommu_domain *d, uint8_t bus, uint8_t devfn)
{
    (void)d;
    (void)bus;
    (void)devfn;
    attaches++;
    return 0;
}

void iommu_detach(struct iommu_domain *d, uint8_t bus, uint8_t devfn)
{
    (void)d;
    (void)bus;
    (void)devfn;
    detaches++;
}

/** The GPU's relaxable RMRR, the network card's direct one, and every
 * device's message window. */
int iommu_reserved_regions(uint8_t bus, uint8_t devfn, struct iommu_region *out, int max)
{
    int n = 0;

    (void)max;
    if (bus == 0 && devfn == 0x10) {
        out[n].start = 0xA0000000u;
        out[n].end = 0xA1FFFFFFu;
        out[n++].type = IOMMU_RESV_DIRECT_RELAXABLE;
    }
    if (bus == 2) {
        out[n].start = 0xB0000000u;
        out[n].end = 0xB00FFFFFu;
        out[n++].type = IOMMU_RESV_DIRECT;
    }
    out[n].start = 0xFEE00000u;
    out[n].end = 0xFEEFFFFFu;
    out[n++].type = IOMMU_RESV_MSI;
    return n;
}

/* ---- pins ---- */

struct pin {
    uint32_t count;
    uint64_t base;
    int live;
    const struct pin_holder *holder;
};
static int pins;         /* pins held */
static uint32_t pin_gap; /* the page after which the frames jump, or 0 */
static int reaps;        /* calls of vm_file_reap */
static int reap_pins;    /* pins held at the last of them */

/* Run once inside the next pin, as another thread runs while the pin
 * sleeps, or NULL. */
static void (*pin_hook)(void);

int pin_user(struct mm *mm, uintptr_t addr, uintptr_t len, uint32_t flags,
             const struct pin_holder *holder, struct pin **out)
{
    struct pin *p;

    (void)mm;
    CHECK((flags & PIN_IO) != 0, "VFIO accepts device pages beside pinned RAM");
    CHECK(holder->flags == PIN_KEEP_PAST_END,
          "VFIO keeps a mapping past the end of the address space");
    CHECK((flags & PIN_LONGTERM) != 0, "VFIO's pin is long-term, as FOLL_LONGTERM");
    if (addr == 0) {
        return -EFAULT;
    }
    p = (struct pin *)calloc(1, sizeof(*p));
    p->count = (uint32_t)(len / 4096);
    p->base = 0x200000000ull + addr;
    p->live = 1;
    p->holder = holder;
    *out = p;
    pins++;
    if (pin_hook != NULL) {
        void (*hook)(void) = pin_hook;

        pin_hook = NULL;
        hook();
    }
    return 0;
}

void unpin(struct pin *pin)
{
    free(pin);
    pins--;
}

/** Count the releases of the files the pins held, which VFIO makes at the
 * end of each operation that unpins, and the pins left at the last. Linux
 * unpins inside iommu->lock, which may sleep, in the unmap
 * (vfio_iommu_type1.c:1409), the detach (:2508), the release (:2601) and a
 * failed map (:1490); Rach2's unpin may not, and releases the files after. */
void vm_file_reap(void)
{
    reaps++;
    reap_pins = pins;
}

int pin_live(const struct pin *pin)
{
    return pin->live;
}

uint32_t pin_count(const struct pin *pin)
{
    return pin->count;
}

uint32_t pin_ram_count(const struct pin *pin)
{
    return pin->count;
}

uintptr_t pin_phys(const struct pin *pin, uint32_t i)
{
    if (!pin->live || i >= pin->count) {
        return 0;
    }
    return (uintptr_t)(pin->base + (uint64_t)i * 4096 +
                       (pin_gap != 0 && i >= pin_gap ? 0x100000u : 0));
}

/** An unmap of the pinned range, as the memory manager makes it. */
static void pin_take_back(struct pin *pin)
{
    pin->holder->release(pin, pin->holder->owner);
    pin->live = 0;
}

/* ---- eventfds, vectors, user memory, the heap, the log ---- */

struct eventfd {
    uint64_t count;
    int refs;
    struct eventfd_hook *hooks;
};
static struct eventfd evs[8]; /* descriptors 100 to 107, and 1100 to 1107 */
static int in_eventfd_hook;
static int irq_wakes;

/** The eventfd of descriptor `fd`; 1100 to 1107 name the same eventfds as
 * 100 to 107, so that a row sees a descriptor above a byte read whole. */
struct eventfd *eventfd_get(int fd, int32_t *err)
{
    if (fd >= 1100 && fd < 1108) {
        fd -= 1000;
    }
    if (fd < 100 || fd >= 108) {
        *err = -EBADF;
        return NULL;
    }
    evs[fd - 100].refs++;
    return &evs[fd - 100];
}

void eventfd_put(struct eventfd *ev)
{
    ev->refs--;
}

void eventfd_signal_except(struct eventfd *ev, const struct eventfd_hook *skip)
{
    struct eventfd_hook *h;

    CHECK(!in_eventfd_hook, "the trigger is signalled outside the unmask hook");
    if (in_eventfd_hook) {
        return;
    }
    ev->count++;
    for (h = ev->hooks; h != NULL; h = h->next) {
        int take;

        if (h == skip) {
            continue;
        }
        in_eventfd_hook = 1;
        take = h->signal(h);
        in_eventfd_hook = 0;
        if (take) {
            ev->count = 0;
        }
    }
}

void eventfd_signal(struct eventfd *ev)
{
    eventfd_signal_except(ev, NULL);
}

int eventfd_hook_add(struct eventfd *ev, struct eventfd_hook *h)
{
    h->next = ev->hooks;
    ev->hooks = h;
    return ev->count > 0;
}

int eventfd_hook_remove(struct eventfd *ev, struct eventfd_hook *h, uint64_t *cnt)
{
    struct eventfd_hook **pp;

    (void)cnt;
    for (pp = &ev->hooks; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == h) {
            *pp = h->next;
            return 1;
        }
    }
    return 0;
}

static isr_handler_t handlers[256];
static int vector_taken[256];

int isr_alloc_vector(void)
{
    int v;

    for (v = 48; v < 240; v++) {
        if (!vector_taken[v]) {
            vector_taken[v] = 1;
            return v;
        }
    }
    return -1;
}

/* A message sent before its vector's removal that another processor has
 * taken and not yet run, which the sync runs; a vector given back first
 * counts in `freed_in_flight`. */
static int in_flight[256];
static int freed_in_flight;

static void raise(int vector);

void isr_vector_sync(int vector)
{
    if (in_flight[vector]) {
        in_flight[vector] = 0;
        raise(vector);
    }
}

void isr_free_vector(int vector)
{
    freed_in_flight += in_flight[vector];
    in_flight[vector] = 0;
    vector_taken[vector] = 0;
    handlers[vector] = NULL;
}

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)name;
    handlers[vector] = handler;
}

/** The vectors VFIO holds now. */
static int vectors_held(void)
{
    int v, n = 0;

    for (v = 48; v < 240; v++) {
        n += vector_taken[v];
    }
    return n;
}

/** Raise `vector`, as the router does. */
static void raise(int vector)
{
    isr_params_t p;

    k_memset(&p, 0, sizeof(p));
    p.vector = (uint64_t)vector;
    if (handlers[vector] != NULL) {
        handlers[vector](&p);
    }
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if (dst == NULL) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (src == NULL) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

int strncpy_from_user(char *dst, const char __user *src, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        dst[i] = src[i];
        if (src[i] == '\0') {
            return (int)i;
        }
    }
    return -ENAMETOOLONG;
}

void *kmalloc(size_t size)
{
    return malloc(size != 0 ? size : 1);
}

void *kzalloc(size_t size)
{
    return calloc(1, size != 0 ? size : 1);
}

void kfree(void *p)
{
    free(p);
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* ---- /sys and /dev ---- */

#define NNODES 256

/* A node of the model's /sys: a directory, an attribute or a link. */
struct snode {
    int parent;
    char name[32];
    const struct sysfs_attr *attr;
    int arg;
    int target;
    int live;
};
static struct snode snodes[NNODES];
static int nsnodes = 1;

/** A new node below `parent`: its handle. */
static int snode_add(int parent, const char *name)
{
    struct snode *s = &snodes[nsnodes];

    k_memset(s, 0, sizeof(*s));
    s->parent = parent;
    k_memcpy(s->name, name, k_strlen(name) + 1);
    s->live = 1;
    return nsnodes++;
}

int sysfs_anchor(int node, int key)
{
    char name[32] = "anchor-";
    int i;

    (void)k_itoa(node * 0x10000 + key, name + 7, sizeof(name) - 7, 'x', false);
    for (i = 1; i < nsnodes; i++) {
        if (snodes[i].parent == 0 && strcmp(snodes[i].name, name) == 0) {
            return i;
        }
    }
    return snode_add(0, name);
}

int sysfs_add_dir(int parent, const char *name)
{
    return snode_add(parent, name);
}

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    int h = snode_add(parent, attr->name);

    (void)obj;
    snodes[h].attr = attr;
    snodes[h].arg = arg;
    return h;
}

int sysfs_add_link(int parent, const char *name, int target)
{
    int h = snode_add(parent, name);

    snodes[h].target = target;
    return h;
}

void sysfs_remove(int handle)
{
    snodes[handle].live = 0;
}

int sysfs_node_child(int dir, const char *name, int len)
{
    int i;

    for (i = 1; i < nsnodes; i++) {
        if (snodes[i].live && snodes[i].parent == dir && (int)k_strlen(snodes[i].name) == len &&
            memcmp(snodes[i].name, name, (unsigned long)len) == 0) {
            return i;
        }
    }
    return -ENOENT;
}

/** The live child `name` of `dir`, or 0. */
static int child(int dir, const char *name)
{
    int h = sysfs_node_child(dir, name, (int)k_strlen(name));

    return h > 0 ? h : 0;
}

/** What the attribute `h` shows, NUL-terminated, in `buf`. */
static const char *show(int h, char *buf, uint32_t cap)
{
    int32_t n = h > 0 && snodes[h].attr->show != NULL
                    ? snodes[h].attr->show(NULL, snodes[h].arg, buf, cap - 1)
                    : -1;

    buf[n >= 0 && (uint32_t)n < cap ? n : 0] = '\0';
    return buf;
}

/** A write of `text` to the attribute `h`: its result. */
static int32_t store(int h, const char *text)
{
    return snodes[h].attr->store(NULL, snodes[h].arg, text, (uint32_t)k_strlen(text));
}

static struct devfs_node *dev_nodes[80];
static int ndev_nodes;

int32_t devfs_node_register(struct devfs_node *node)
{
    dev_nodes[ndev_nodes++] = node;
    return 0;
}

void devfs_node_unregister(struct devfs_node *node)
{
    (void)node;
}

/** The registered node `name`, or NULL. */
static struct devfs_node *dev_node(const char *name)
{
    int i;

    for (i = 0; i < ndev_nodes; i++) {
        if (strcmp(dev_nodes[i]->name, name) == 0) {
            return dev_nodes[i];
        }
    }
    return NULL;
}

/* ---- memory mappings, descriptors and the rest of hw.c ---- */

static const struct vm_device_ops *mapped_ops;
static void *mapped_obj;
static int zaps;
static uint32_t zap_pgoff, zap_count; /* the last zap's pages */

int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    (void)vma;
    CHECK(cache == VM_PROT_UC, "a BAR is mapped uncached");
    mapped_ops = ops;
    mapped_obj = obj;
    return 0;
}

void paging_zap_device(const void *obj, uint32_t pgoff, uint32_t count)
{
    (void)obj;
    zap_pgoff = pgoff;
    zap_count = count;
    zaps++;
}

static fd_entry_t fds[64];
static struct open_file ofds[64];
static int nfds = 3;
static int unsafe_option;
static unsigned int slept;
static int pause_power_wait;

/* The OpRegion at the GPU's ASLS. */
#define ASLS 0xDAF00000u
static uint8_t opregion[0x2000];

static unsigned int udelayed; /* microseconds VFIO spun */

void vfio_hw_udelay(unsigned int us)
{
    udelayed += us;
}

void vfio_hw_msleep(unsigned int ms)
{
    slept += ms;
    if (pause_power_wait && ms == PCI_PM_D3HOT_WAIT && fiber_running() == 0) {
        fiber_yield();
    }
}

int vfio_hw_irq_start(void)
{
    return 0;
}

void vfio_hw_irq_wake(void)
{
    irq_wakes++;
}

void mutex_init_key(struct mutex *m, const char *name, const struct lockdep_key *key)
{
    (void)name;
    (void)key;
    m->owner = 0;
}

/** Model a sleeping mutex by giving the test the next turn. */
void mutex_lock(struct mutex *m)
{
    uintptr_t owner = (uintptr_t)(fiber_running() + 2);

    while (m->owner != 0) {
        if (m->owner == owner || fiber_running() < 0) {
            CHECK(0, "the caller can wait for the device lock");
            return;
        }
        fiber_yield();
    }
    m->owner = owner;
}

void mutex_unlock(struct mutex *m)
{
    CHECK(m->owner == (uintptr_t)(fiber_running() + 2), "the caller holds the device lock");
    m->owner = 0;
}

struct mm *vfio_hw_mm(void)
{
    static int mm;

    return (struct mm *)(void *)&mm;
}

int32_t vfio_hw_fd_new(const struct file_ops *ops, void *priv)
{
    int fd = nfds++;

    fds[fd].type = FD_TYPE_ANON;
    fds[fd].ops = ops;
    fds[fd].priv = priv;
    fds[fd].ofd = &ofds[fd];
    fds[fd].flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
    return fd;
}

void *vfio_hw_fd_object(int fd, const struct file_ops *ops)
{
    return fd >= 0 && fd < nfds && fds[fd].ops == ops ? fds[fd].priv : NULL;
}

uintptr_t vfio_hw_map(uint64_t phys, uint32_t size)
{
    (void)size;
    return phys == ASLS ? (uintptr_t)opregion : 0;
}

void vfio_hw_unmap(uintptr_t va, uint32_t size)
{
    (void)va;
    (void)size;
}

int vfio_hw_param_bool(const char *key)
{
    return unsafe_option && strcmp(key, "vfio_iommu_type1.allow_unsafe_interrupts") == 0;
}

/** Open the registered node `name` as a new descriptor: the descriptor,
 * or the open's error. */
static int open_node(const char *name)
{
    struct devfs_node *n = dev_node(name);
    int fd = nfds;
    int32_t r;

    fds[fd].type = FD_TYPE_DEVICE;
    fds[fd].ops = n->ops;
    fds[fd].ofd = &ofds[fd];
    r = n->open(n, &fds[fd], 0);
    if (r != 0) {
        return r;
    }
    nfds++;
    return fd;
}

/** An ioctl on descriptor `fd`. */
static int32_t ioc(int fd, uint32_t request, uintptr_t arg)
{
    return fds[fd].ops->ioctl(&fds[fd], request, arg);
}

/** The last close of descriptor `fd`. */
static void closefd(int fd)
{
    fds[fd].ops->close(&fds[fd]);
    fds[fd].ops = NULL;
}

/** pread on a device's descriptor at `pos`. */
static int32_t pread_at(int fd, uint64_t pos, void *buf, uint32_t n)
{
    ofds[fd].offset = pos;
    return fds[fd].ops->read(&fds[fd], (char __user *)buf, n);
}

/** pwrite on a device's descriptor at `pos`. */
static int32_t pwrite_at(int fd, uint64_t pos, const void *buf, uint32_t n)
{
    ofds[fd].offset = pos;
    return fds[fd].ops->write(&fds[fd], (const char __user *)buf, n);
}

/* ---- the machine ---- */

/** Build the bus of the opening comment. */
static void build(void)
{
    struct fn *m;

    m = add_fn(0, 0, 0, 0x0A048086, 0x0600, 0);
    m->owner = PCI_OWNER_KERNEL;
    m = add_fn(0, 2, 0, 0x0A168086, 0x0300, 0);
    m->unit = 0;
    m->method = PCI_RESET_AF_FLR;
    m->cfg[0x3C] = 11;
    m->cfg[0x3D] = 1;
    add_bar(m, 0, 0xF0000000u, 0x400000u, 0, 1);
    add_bar(m, 2, 0xE0000000u, 0x10000000u, 0, 1);
    add_bar(m, 4, 0x3000u, 64, 1, 0);
    add_cap(m, PCI_CAP_ID_MSI, 0x90);
    put16(m->cfg + 0x92, 0x0082); /* 64-bit, two messages */
    add_cap(m, PCI_CAP_ID_PM, 0xD0);
    add_cap(m, PCI_CAP_ID_AF, 0xA4);
    m->cfg[0xA6] = 0x03;
    put32(m->cfg + 0xFC, ASLS);
    m = add_fn(0, 0x14, 0, 0x9C318086, 0x0C03, 0);
    m->method = PCI_RESET_PM;
    add_bar(m, 0, 0xF7100000u, 0x10000u, 0, 1);
    add_cap(m, PCI_CAP_ID_MSIX, 0x80);
    put16(m->cfg + 0x82, 7); /* eight entries */
    put32(m->cfg + 0x84, 0x2000);
    put32(m->cfg + 0x88, 0x3000);
    add_cap(m, PCI_CAP_ID_PM, 0xD0);
    m = add_fn(0, 0x1C, 0, 0x9C108086, 0x0604, 1);
    m->cfg[0x19] = 2;
    add_cap(m, PCI_CAP_ID_PCIE, 0x40);
    put16(m->cfg + 0x42, 0x0042); /* a root port */
    m->owner = PCI_OWNER_KERNEL;
    m = add_fn(0, 0x1D, 0, 0x12348086, 0x0604, 1);
    m->cfg[0x19] = 3;
    add_cap(m, PCI_CAP_ID_PCIE, 0x40);
    put16(m->cfg + 0x42, 0x0042);
    add_fn(0, 0x1F, 0, 0x9C438086, 0x0601, 0x80);
    m = add_fn(0, 0x1F, 2, 0x9C038086, 0x0106, 0);
    m->owner = PCI_OWNER_KERNEL;
    add_fn(0, 0x1F, 3, 0x9C228086, 0x0C05, 0);
    m = add_fn(2, 0, 0, 0x0034168C, 0x0280, 0);
    m->method = PCI_RESET_FLR;
    add_bar(m, 0, 0xF7000000u, 0x80000u, 0, 1);
    add_cap(m, PCI_CAP_ID_PCIE, 0x40);
    put16(m->cfg + 0x42, 0x0002); /* an endpoint */
    put32(m->cfg + 0x100, 0x00010001u);
    add_fn(3, 0, 0, 0x816810EC, 0x0200, 0);
    k_memcpy(opregion, "IntelGraphicsMem", 16);
    put32(opregion + 0x10, 8);
    put16(opregion + 0x16, 0x0200);
}

/** The model's function of the vfio device named `name`. */
static struct vfio_device *vdev(const char *name)
{
    return vfio_device_by_name(name, (int)k_strlen(name));
}

/* ---- groups, /sys and binding ---- */

/** The groups and their entries of /sys and /dev, and binding. */
static void group_rows(void)
{
    static struct vfio_container owner; /* stands for a container that holds group 0 */
    char buf[256];
    int groups, g0, gpu, kernel;

    CHECK(vfio_groups_build() == 7, "seven groups");
    CHECK(vdev("0000:00:02.0")->group == 0, "the GPU, alone on unit 0, is group 0");
    CHECK(vdev("0000:00:00.0")->group == 1, "unit 1's groups follow in bus order");
    CHECK(vdev("0000:02:00.0")->group == 6, "the PCH quirk isolates the card behind 00:1c.0");
    CHECK(vdev("0000:03:00.0")->group == vdev("0000:00:1d.0")->group,
          "a root port without ACS joins the group of what is behind it");
    CHECK(vdev("0000:00:1f.2")->group == vdev("0000:00:1f.0")->group &&
              vdev("0000:00:1f.3")->group == vdev("0000:00:1f.0")->group,
          "the functions of a slot without ACS share a group");
    CHECK(vfio_nodes_start() == 0 && vfio_sysfs_start() == 0, "nodes and /sys start");
    CHECK(dev_node("vfio/vfio") != NULL && dev_node("vfio/vfio")->major == 10 &&
              dev_node("vfio/vfio")->minor == 196 && dev_node("vfio/vfio")->mode == 0666,
          "/dev/vfio/vfio is 10:196, 0666");
    CHECK(dev_node("vfio/0") != NULL && dev_node("vfio/6") != NULL && dev_node("vfio/7") == NULL,
          "/dev/vfio/N for each group");
    groups = child(sysfs_anchor(SYSFS_KERNEL, 0), "iommu_groups");
    g0 = child(groups, "0");
    CHECK(strcmp(show(child(g0, "type"), buf, sizeof(buf)), "identity\n") == 0, "type");
    CHECK(snodes[child(g0, "type")].attr->mode == 0644u, "type is 0644, as Linux's");
    CHECK(store(child(g0, "type"), "identity\n") == 9 && store(child(g0, "type"), "auto") == 4,
          "a group with no driver takes identity and auto, which keep its domain");
    CHECK(store(child(g0, "type"), "DMA") == -EINVAL &&
              store(child(g0, "type"), "DMA-FQ\n") == -EINVAL,
          "and refuses DMA and DMA-FQ, which a host without IOMMU DMA cannot give");
    CHECK(store(child(g0, "type"), "blocked") == -EINVAL &&
              store(child(g0, "type"), "DMA\n\n") == -EINVAL,
          "and a word it does not know, with sysfs_streq's one newline at most");
    writer_caps_lost = 1u << CAP_SYS_RAWIO;
    CHECK(store(child(g0, "type"), "identity") == -EACCES,
          "a writer without CAP_SYS_RAWIO is EACCES");
    writer_caps_lost = 1u << CAP_SYS_ADMIN;
    CHECK(store(child(g0, "type"), "bogus") == -EACCES,
          "and one without CAP_SYS_ADMIN, before the word");
    writer_caps_lost = 0;
    CHECK(strcmp(show(child(g0, "reserved_regions"), buf, sizeof(buf)),
                 "0x00000000a0000000 0x00000000a1ffffff direct-relaxable\n"
                 "0x00000000fee00000 0x00000000feefffff msi\n") == 0,
          "reserved_regions, sorted, with their kinds");
    gpu = sysfs_anchor(SYSFS_PCI_DEVICE, 0x0010);
    CHECK(child(child(g0, "devices"), "0000:00:02.0") != 0, "the group's devices link");
    CHECK(snodes[child(gpu, "iommu_group")].target == g0, "iommu_group links to the group");
    CHECK(strcmp(show(child(gpu, "driver_override"), buf, sizeof(buf)), "(null)\n") == 0,
          "driver_override shows (null) unset");
    CHECK(vfio_override_allows(0x0010, "i915") && vfio_override_allows(0x7f00, "e1000"),
          "an unset override, or a function no group has, lets any driver bind");
    CHECK(store(child(gpu, "driver_override"), "(null)") == 6 &&
              !vfio_override_allows(0x0010, "i915") && vfio_override_allows(0x0010, "(null)"),
          "the text (null) names a driver, though it shows as an unset override does");
    kernel = child(child(child(sysfs_anchor(SYSFS_BUS_PCI, 0), "drivers"), "vfio-pci"), "bind");
    CHECK(store(kernel, "0000:00:02.0\n") == -ENODEV, "bind refuses a device that does not match");
    CHECK(store(child(gpu, "driver_override"), "vfio-pci\n") == 9, "driver_override takes a name");
    CHECK(strcmp(show(child(gpu, "driver_override"), buf, sizeof(buf)), "vfio-pci\n") == 0,
          "driver_override shows the name");
    CHECK(vfio_override_allows(0x0010, "vfio-pci") && !vfio_override_allows(0x0010, "i915"),
          "and lets that driver bind, and no other");
    CHECK(store(kernel, "0000:00:02.0\n") == 13 && vdev("0000:00:02.0")->bound,
          "bind gives the device to vfio-pci");
    CHECK(strcmp(pci_owner(0, 0x10), "vfio-pci") == 0, "the device's owner is vfio-pci");
    CHECK(child(gpu, "driver") != 0, "the bound device has a driver link");
    CHECK(store(child(sysfs_anchor(SYSFS_PCI_DEVICE, 0x00FA), "driver_override"), "vfio-pci") == 8,
          "driver_override of the SATA function");
    CHECK(store(kernel, "0000:00:1f.2") == -EBUSY, "bind refuses a device a kernel driver has");
    CHECK(vfio_group_viable(&vfio_groups[0]), "the GPU's group is viable");
    CHECK(!vfio_group_viable(&vfio_groups[vdev("0000:00:1f.0")->group]),
          "a group with a kernel driver's function is not");
    CHECK(vfio_group_viable(&vfio_groups[6]) && vfio_group_viable(&vfio_groups[3]),
          "a bridge with a kernel driver keeps its group viable");
    k_itoa(vdev("0000:00:1f.0")->group, buf, sizeof(buf), 'd', false);
    CHECK(store(child(child(groups, buf), "type"), "identity") == -EPERM &&
              store(child(child(groups, buf), "type"), "DMA") == -EPERM,
          "a group whose function a kernel driver has is EPERM, as Linux's owner_cnt");
    CHECK(store(child(child(groups, buf), "type"), "bogus") == -EINVAL, "after the word is read");
    CHECK(store(child(g0, "type"), "identity") == 8,
          "vfio-pci's function leaves its group's type writable, as driver_managed_dma");
    vfio_groups[0].container = &owner;
    CHECK(store(child(g0, "type"), "identity") == -EPERM, "a group in a container is EPERM");
    vfio_groups[0].container = NULL;
}

/** new_id, remove_id, unbind and drivers_probe. */
static void binding_rows(void)
{
    int drv = child(child(sysfs_anchor(SYSFS_BUS_PCI, 0), "drivers"), "vfio-pci");
    int probe = child(sysfs_anchor(SYSFS_BUS_PCI, 0), "drivers_probe");

    CHECK(store(child(drv, "new_id"), "168c 0034\n") == 10 && vdev("0000:02:00.0")->bound,
          "new_id binds every free device of the ids");
    CHECK(store(child(drv, "remove_id"), "168c 0034") == 9, "remove_id forgets them");
    CHECK(store(child(drv, "remove_id"), "168c 0034") == -ENODEV, "and refuses ids not given");
    CHECK(store(child(drv, "unbind"), "0000:02:00.0") == 12 && !vdev("0000:02:00.0")->bound &&
              pci_owner(2, 0) == NULL,
          "unbind takes the device back");
    CHECK(store(probe, "0000:02:00.0\n") == 13 && !vdev("0000:02:00.0")->bound &&
              virtio_offers == 1 && virtio_offered == 0x0200,
          "drivers_probe offers a function vfio-pci does not match to virtio-pci");
    CHECK(store(child(sysfs_anchor(SYSFS_PCI_DEVICE, 0x0200), "driver_override"), "vfio-pci") ==
                  8 &&
              store(probe, "0000:02:00.0") == 12 && vdev("0000:02:00.0")->bound,
          "drivers_probe binds by driver_override");
    CHECK(virtio_offers == 1, "and offers virtio-pci nothing it binds");
    CHECK(store(child(sysfs_anchor(SYSFS_PCI_DEVICE, 0x00A0), "driver_override"), "vfio-pci") ==
                  8 &&
              store(probe, "0000:00:14.0") == 12 && vdev("0000:00:14.0")->bound,
          "and the xHCI");
}

/* ---- groups and containers ---- */

static int container, group0, device;

static struct vfio_iommu_type1_dma_map raced = {sizeof(raced), 1, 0x400000, 0x400000, 0x1000};
static int32_t raced_r; /* map_same_iova's result */
static int32_t unset_r; /* unset_last_group's result */
static int unset_reaps; /* reaps when unset_last_group returned */

/** Another thread's MAP_DMA of `raced`'s IOVA while a pin sleeps. Linux's
 * vfio_dma_do_map holds iommu->lock from its overlap check through its pin
 * (vfio_iommu_type1.c:1572, :1589, :1655), so there the second map waits;
 * Rach2's pin sleeps without the big kernel lock, so MAP_DMA checks again. */
static void map_same_iova(void)
{
    raced_r = ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&raced);
}

/** Another thread's UNSET_CONTAINER of the last group while a pin sleeps.
 * Linux's detach waits for the map on iommu->lock (vfio_iommu_type1.c:2464),
 * and the unpin of every mapping follows (:2508); here MAP_DMA finds the
 * driver gone after its pin. */
static void unset_last_group(void)
{
    unset_r = ioc(group0, VFIO_GROUP_UNSET_CONTAINER, 0);
    unset_reaps = reaps;
}

/** The container and group ioctls up to SET_IOMMU. */
static void container_rows(void)
{
    struct vfio_group_status st = {sizeof(st), 0};
    struct vfio_iommu_type1_dma_map map = {sizeof(map), 3, 0x10000, 0x100000, 0x4000};
    int32_t fd;

    container = open_node("vfio/vfio");
    CHECK(ioc(container, VFIO_GET_API_VERSION, 0) == 0, "API version 0");
    CHECK(ioc(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU) == 1 &&
              ioc(container, VFIO_CHECK_EXTENSION, VFIO_TYPE1v2_IOMMU) == 1 &&
              ioc(container, VFIO_CHECK_EXTENSION, VFIO_UNMAP_ALL) == 1,
          "Type1, v2 and UNMAP_ALL");
    CHECK(ioc(container, VFIO_CHECK_EXTENSION, VFIO_DMA_CC_IOMMU) == 0 &&
              ioc(container, VFIO_CHECK_EXTENSION, VFIO_UPDATE_VADDR) == 0,
          "neither cache coherence nor UPDATE_VADDR");
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&map) == -EINVAL, "MAP_DMA needs a driver");
    CHECK(ioc(container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) == -EINVAL, "SET_IOMMU needs a group");
    group0 = open_node("vfio/0");
    CHECK(open_node("vfio/0") == -EBUSY, "a group opens once at a time");
    CHECK(ioc(group0, VFIO_GROUP_GET_STATUS, (uintptr_t)&st) == 0 &&
              st.flags == VFIO_GROUP_FLAGS_VIABLE,
          "the group is viable and in no container");
    fd = container;
    CHECK(ioc(group0, VFIO_GROUP_SET_CONTAINER, (uintptr_t)&fd) == 0, "SET_CONTAINER");
    CHECK(ioc(group0, VFIO_GROUP_GET_STATUS, (uintptr_t)&st) == 0 &&
              st.flags == (VFIO_GROUP_FLAGS_VIABLE | VFIO_GROUP_FLAGS_CONTAINER_SET),
          "the group is in the container");
    CHECK(ioc(group0, VFIO_GROUP_GET_DEVICE_FD, (uintptr_t)"0000:00:02.0") == -EINVAL,
          "GET_DEVICE_FD needs the IOMMU set");
    CHECK(ioc(container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) == -EPERM,
          "SET_IOMMU refuses a host without interrupt remapping");
    unsafe_option = 1;
    CHECK(ioc(container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) == 0 && attaches == 1 && domains == 1,
          "allow_unsafe_interrupts lets it, and the GPU goes into the domain");
    CHECK(ioc(container, VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) == -EBUSY, "set once");
}

/** VFIO_IOMMU_GET_INFO and its capabilities. */
static void iommu_info_rows(void)
{
    uint8_t info[256];
    struct vfio_iommu_type1_info *ti = (struct vfio_iommu_type1_info *)(void *)info;
    struct vfio_iommu_type1_info_cap_iova_range *cap;
    struct vfio_iova_range *r;

    ti->argsz = 16;
    CHECK(ioc(container, VFIO_IOMMU_GET_INFO, (uintptr_t)ti) == 0 &&
              ti->flags == VFIO_IOMMU_INFO_PGSIZES && ti->iova_pgsizes == 0x40201000ull,
          "GET_INFO's page sizes");
    ti->argsz = 24;
    CHECK(ioc(container, VFIO_IOMMU_GET_INFO, (uintptr_t)ti) == 0 && ti->argsz == 24 + 48 + 16 &&
              ti->cap_offset == 0 && (ti->flags & VFIO_IOMMU_INFO_CAPS),
          "GET_INFO grows argsz for its capabilities");
    ti->argsz = sizeof(info);
    CHECK(ioc(container, VFIO_IOMMU_GET_INFO, (uintptr_t)ti) == 0 && ti->cap_offset == 24,
          "GET_INFO's capabilities follow it");
    cap = (struct vfio_iommu_type1_info_cap_iova_range *)(void *)(info + 24);
    r = (struct vfio_iova_range *)(void *)(info + 24 + sizeof(*cap));
    CHECK(cap->header.id == VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE && cap->header.next == 72 &&
              cap->nr_iovas == 2,
          "two IOVA ranges");
    CHECK(r[0].start == 0 && r[0].end == 0xFEDFFFFFu && r[1].start == 0xFEF00000u &&
              r[1].end == (1ull << 39) - 1,
          "the message window is taken out, the relaxable RMRR kept");
    CHECK(((struct vfio_info_cap_header *)(void *)(info + 72))->id ==
                  VFIO_IOMMU_TYPE1_INFO_DMA_AVAIL &&
              ((struct vfio_iommu_type1_info_dma_avail *)(void *)(info + 72))->avail == 65535,
          "65535 mappings available");
}

/** MAP_DMA and UNMAP_DMA. */
static void dma_rows(void)
{
    struct vfio_iommu_type1_dma_map map = {sizeof(map), 3, 0x10000, 0x100000, 0x4000};
    struct vfio_iommu_type1_dma_unmap um = {sizeof(um), 0, 0x101000, 0x1000};
    struct vfio_container *c = (struct vfio_container *)fds[container].priv;
    int32_t fd = container;

    pin_gap = 2;
    reaps = 0;
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&map) == 0 && nmaps == 2 &&
              maps[0].iova == 0x100000 && maps[0].size == 0x2000 &&
              maps[0].phys == 0x200010000ull && maps[1].iova == 0x102000 &&
              maps[1].phys == 0x200112000ull && maps[0].prot == (IOMMU_READ | IOMMU_WRITE),
          "MAP_DMA maps each contiguous run of the pinned pages");
    pin_gap = 0;
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&map) == -EEXIST, "an overlap is EEXIST");
    map.iova = 0xFEE00000u;
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&map) == -EINVAL,
          "the message window is no IOVA");
    map.iova = 0x200800;
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&map) == -EINVAL, "an unaligned IOVA");
    map.iova = 0x200000;
    map.flags = 0;
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&map) == -EINVAL, "no access");
    map.flags = 1;
    c->ndma = VFIO_DMA_MAX;
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&map) == -ENOSPC, "past the limit");
    c->ndma = 1;
    CHECK(ioc(container, VFIO_IOMMU_UNMAP_DMA, (uintptr_t)&um) == -EINVAL,
          "v2 refuses to split a mapping");
    um.iova = 0x100000;
    um.size = 0x4000;
    CHECK(ioc(container, VFIO_IOMMU_UNMAP_DMA, (uintptr_t)&um) == 0 && um.size == 0x4000 &&
              unmaps == 1 && pins == 0 && reaps == 1 && reap_pins == 0,
          "UNMAP_DMA removes the mapping, unpins it, and then releases the pin's files");
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&map) == 0 && maps[2].prot == IOMMU_READ,
          "a read-only mapping");
    map.iova = 0x300000;
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&map) == 0, "a second mapping");
    pin_take_back(c->dmas->pin);
    CHECK(unmaps == 2 && unmapped == 0x8000, "an unmap of the pinned memory unmaps the IOVA");
    um.flags = VFIO_DMA_UNMAP_FLAG_ALL;
    CHECK(ioc(container, VFIO_IOMMU_UNMAP_DMA, (uintptr_t)&um) == -EINVAL,
          "UNMAP_ALL takes no range");
    um.iova = um.size = 0;
    CHECK(ioc(container, VFIO_IOMMU_UNMAP_DMA, (uintptr_t)&um) == 0 && um.size == 0x8000 &&
              unmaps == 3 && pins == 0 && c->ndma == 0,
          "UNMAP_ALL removes every mapping, the one taken back without a second unmap");
    CHECK(ioc(vfio_groups[5].open ? 0 : open_node("vfio/5"), VFIO_GROUP_SET_CONTAINER,
              (uintptr_t)&fd) == -EPERM,
          "a group that is not viable is refused");
}

/** A MAP_DMA of the IOVA that another thread maps while the pin sleeps. */
static void dma_race_rows(void)
{
    struct vfio_iommu_type1_dma_unmap um = {sizeof(um), VFIO_DMA_UNMAP_FLAG_ALL, 0, 0};
    struct vfio_container *c = (struct vfio_container *)fds[container].priv;

    reaps = 0;
    pin_hook = map_same_iova;
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&raced) == -EEXIST && raced_r == 0 &&
              c->ndma == 1 && pins == 1 && reaps == 1,
          "a MAP_DMA whose pin slept checks the IOVA again and fails, as Linux's checks it "
          "under the lock its pin holds");
    CHECK(ioc(container, VFIO_IOMMU_UNMAP_DMA, (uintptr_t)&um) == 0 && um.size == 0x1000 &&
              pins == 0 && c->ndma == 0,
          "the map made meanwhile stays until its unmap");
}

/* ---- the device ---- */

/** The device's descriptor and its regions' information. */
static void region_rows(void)
{
    struct vfio_device_info di = {sizeof(di), 0, 0, 0, 0, 0};
    struct vfio_region_info ri;
    uint8_t buf[64];
    struct vfio_region_info_cap_type *ct = (struct vfio_region_info_cap_type *)(void *)(buf + 32);
    struct vfio_region_info *rp = (struct vfio_region_info *)(void *)buf;

    device = ioc(group0, VFIO_GROUP_GET_DEVICE_FD, (uintptr_t)"0000:00:02.0");
    CHECK(device > 0 && fns[1].resets == 1, "the device opens with a reset");
    CHECK(ioc(group0, VFIO_GROUP_GET_DEVICE_FD, (uintptr_t)"0000:00:14.0") == -ENODEV,
          "a device of another group is refused");
    CHECK(ioc(device, VFIO_DEVICE_GET_INFO, (uintptr_t)&di) == 0 &&
              di.flags == (VFIO_DEVICE_FLAGS_PCI | VFIO_DEVICE_FLAGS_RESET) &&
              di.num_regions == 12 && di.num_irqs == 5,
          "GET_INFO: the IGD's three regions after the nine");
    ri.argsz = sizeof(ri);
    ri.index = 0;
    CHECK(ioc(device, VFIO_DEVICE_GET_REGION_INFO, (uintptr_t)&ri) == 0 && ri.size == 0x400000 &&
              ri.offset == 0 &&
              ri.flags == (VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE |
                           VFIO_REGION_INFO_FLAG_MMAP),
          "BAR 0");
    ri.index = 1;
    CHECK(ioc(device, VFIO_DEVICE_GET_REGION_INFO, (uintptr_t)&ri) == 0 && ri.size == 0 &&
              ri.offset == 1ull << 40,
          "a 64-bit BAR's upper half is empty");
    ri.index = 4;
    CHECK(ioc(device, VFIO_DEVICE_GET_REGION_INFO, (uintptr_t)&ri) == 0 && ri.size == 64 &&
              ri.flags == (VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE),
          "an I/O BAR is not mapped");
    ri.index = VFIO_PCI_CONFIG_REGION_INDEX;
    CHECK(ioc(device, VFIO_DEVICE_GET_REGION_INFO, (uintptr_t)&ri) == 0 && ri.size == 256 &&
              ri.offset == 7ull << 40,
          "the configuration space");
    ri.index = VFIO_PCI_VGA_REGION_INDEX;
    CHECK(ioc(device, VFIO_DEVICE_GET_REGION_INFO, (uintptr_t)&ri) == -EINVAL, "no VGA region");
    k_memset(buf, 0, sizeof(buf));
    rp->argsz = 32;
    rp->index = 9;
    CHECK(ioc(device, VFIO_DEVICE_GET_REGION_INFO, (uintptr_t)rp) == 0 && rp->argsz == 48 &&
              rp->cap_offset == 0 && (rp->flags & VFIO_REGION_INFO_FLAG_CAPS),
          "the OpRegion's information asks for room for its type");
    rp->argsz = sizeof(buf);
    CHECK(ioc(device, VFIO_DEVICE_GET_REGION_INFO, (uintptr_t)rp) == 0 && rp->size == 0x2000 &&
              rp->cap_offset == 32 && rp->offset == 9ull << 40 &&
              rp->flags == (VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_CAPS),
          "the OpRegion region");
    CHECK(ct->header.id == VFIO_REGION_INFO_CAP_TYPE && ct->type == 0x80008086u &&
              ct->subtype == VFIO_REGION_SUBTYPE_INTEL_IGD_OPREGION,
          "its type is Intel's OpRegion");
    rp->index = 11;
    CHECK(ioc(device, VFIO_DEVICE_GET_REGION_INFO, (uintptr_t)rp) == 0 && rp->size == 256 &&
              ct->subtype == VFIO_REGION_SUBTYPE_INTEL_IGD_LPC_CFG,
          "the LPC bridge's region");
    rp->index = 12;
    CHECK(ioc(device, VFIO_DEVICE_GET_REGION_INFO, (uintptr_t)rp) == -EINVAL, "no thirteenth");
}

/** The configuration space and the other regions as the owner reads and
 * writes them. */
static void config_rows(void)
{
    uint32_t v = 0xFFFFFFFFu;
    uint16_t w;
    uint8_t b[16];
    struct fn *m = &fns[1];
    uint64_t cfg = 7ull << 40;

    CHECK(pread_at(device, cfg, &v, 4) == 4 && v == 0x0A168086u, "the identity reads through");
    v = 0xFFFFFFFFu;
    CHECK(pwrite_at(device, cfg + 0x10, &v, 4) == 4 && pread_at(device, cfg + 0x10, &v, 4) == 4 &&
              v == 0xFFC00004u,
          "BAR 0 sized by all ones answers its size, as a device does");
    CHECK(get32(m->cfg + 0x10) == 0xF0000004u, "the device's BAR keeps its address");
    CHECK(pread_at(device, cfg + 0x14, &v, 4) == 4 && v == 0xF0000004u - 0xF0000004u,
          "the upper half was not written");
    w = 0x0406; /* memory, bus master and Interrupt Disable */
    CHECK(pwrite_at(device, cfg + 4, &w, 2) == 2 && pread_at(device, cfg + 4, &w, 2) == 2 &&
              w == 0x0406,
          "the command word reads back as written");
    CHECK((m->cfg[5] & 0x04) && (m->cfg[4] & 0x06) == 0x06, "decoding and mastering go through");
    w = 0x0004;
    v = (uint32_t)zaps;
    CHECK(pwrite_at(device, cfg + 4, &w, 2) == 2 && zaps == (int)v + 1 && zap_pgoff == 0 &&
              zap_count == 6u << VFIO_PCI_PGOFF_SHIFT,
          "turning memory off takes every BAR from the mappings in one walk");
    CHECK((m->cfg[5] & 0x04) != 0, "the line stays off while the owner has no interrupt");
    w = 0x0406;
    (void)pwrite_at(device, cfg + 4, &w, 2);
    b[0] = 0x01;
    CHECK(pwrite_at(device, cfg + 0x92, b, 1) == 1 && pread_at(device, cfg + 0x92, b, 1) == 1 &&
              b[0] == 0x83 && m->cfg[0x92] == 0x82,
          "MSI's enable bit is VFIO's");
    b[0] = 0x0E;
    CHECK(pwrite_at(device, cfg + 0x92, b, 1) == 1 && pread_at(device, cfg + 0x92, b, 1) == 1 &&
              b[0] == 0x82,
          "MSI's capable count cannot be written");
    v = 0;
    CHECK(pwrite_at(device, cfg + 0xFC, &v, 4) == 4 && pread_at(device, cfg + 0xFC, &v, 4) == 4 &&
              v == ASLS,
          "ASLS reads the OpRegion's address and takes no write");
    b[0] = 0x01;
    CHECK(pwrite_at(device, cfg + 0xA8, b, 1) == 1 && m->cfg[0xA8] == 0,
          "the AF capability's FLR bit is taken out");
    b[0] = 0x55;
    CHECK(pwrite_at(device, cfg + 0x60, b, 1) == 1 && m->cfg[0x60] == 0x55,
          "a byte outside every capability goes through");
    CHECK(pread_at(device, cfg + 0x100, b, 4) == -EINVAL, "conventional space ends at 256");
    CHECK(pread_at(device, 9ull << 40, b, 16) == 16 && memcmp(b, "IntelGraphicsMem", 16) == 0,
          "the OpRegion reads with its signature");
    CHECK(pread_at(device, (9ull << 40) + 0x16, &w, 2) == 2 && w == 0x0200, "and its version");
    CHECK(pwrite_at(device, 9ull << 40, b, 4) == -EINVAL, "the OpRegion takes no write");
    CHECK(pread_at(device, 10ull << 40, &v, 4) == 4 && v == 0x0A048086u,
          "the host bridge's region reads its configuration");
    ports[0x3004] = 0x5A;
    CHECK(pread_at(device, (4ull << 40) + 4, b, 1) == 1 && b[0] == 0x5A, "an I/O BAR reads ports");
    v = 0x12345678u;
    CHECK(pwrite_at(device, (4ull << 40) + 0x3C, &v, 4) == 4 && get32(&ports[0x303C]) == v,
          "and writes them");
    CHECK(pwrite_at(device, (4ull << 40) + 0x40, &v, 4) == -EINVAL, "up to its end");
}

static int32_t set_irqs(int fd, uint32_t flags, uint32_t index, uint32_t start, uint32_t count,
                        const int32_t *v);

/** A PCI Express device in a second group of the container. */
static void pcie_rows(void)
{
    struct vfio_group_status st = {sizeof(st), 0};
    int g6 = open_node("vfio/6"), wifi;
    int32_t fd = container, err = 1107;
    uint8_t b = 0x80;

    CHECK(ioc(g6, VFIO_GROUP_SET_CONTAINER, (uintptr_t)&fd) == 0 && attaches == 2,
          "a second group joins the container and its domain");
    CHECK(ioc(g6, VFIO_GROUP_GET_STATUS, (uintptr_t)&st) == 0 && st.flags == 3, "and is set");
    wifi = ioc(g6, VFIO_GROUP_GET_DEVICE_FD, (uintptr_t)"0000:02:00.0");
    CHECK(wifi > 0 && fns[8].resets == 1, "the card opens with a function-level reset");
    CHECK(pwrite_at(wifi, (7ull << 40) + 0x49, &b, 1) == 1 && fns[8].cfg[0x49] == 0,
          "Initiate Function Level Reset is taken out of a write");
    CHECK(vfio_device_by_name("0000:02:00.0", 12)->cfg_size == 4096,
          "a PCI Express device has the extended space");
    CHECK(set_irqs(wifi, VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
                   VFIO_PCI_ERR_IRQ_INDEX, 0, 1, &err) == 0 &&
              evs[7].refs == 1,
          "its error eventfd, its descriptor above a byte read whole");
    closefd(wifi);
    CHECK(fns[8].resets == 2 && evs[7].refs == 0,
          "the card is reset at its release, its error eventfd dropped");
    reaps = 0;
    closefd(g6);
    CHECK(detaches == 1 && domains == 1 && reaps == 1,
          "its group leaves the domain at the last close, and the detach releases the pins' files");
}

/** Mappings of the BARs. */
static void map_rows(void)
{
    vma_t vma;
    uintptr_t phys = 0;
    struct vfio_device *d = vdev("0000:00:02.0");

    k_memset(&vma, 0, sizeof(vma));
    vma.end = 0x2000;
    vma.pgoff = (2u << 28) + 1;
    CHECK(fds[device].ops->mmap(&fds[device], &vma) == 0 && mapped_obj == d && d->refs == 2,
          "BAR 2 is mapped, holding the device");
    CHECK(mapped_ops->page(d, (2u << 28) + 1, &phys) == 0 && phys == 0xE0001000u,
          "a page of the mapping is the BAR's");
    fns[1].cfg[4] &= (uint8_t)~0x02;
    CHECK(mapped_ops->page(d, (2u << 28) + 1, &phys) == -1, "not with memory decoding off");
    fns[1].cfg[4] |= 0x02;
    vma.pgoff = 4u << 28;
    CHECK(fds[device].ops->mmap(&fds[device], &vma) == -EINVAL, "an I/O BAR cannot be mapped");
    vma.pgoff = 0x3FFu;
    CHECK(fds[device].ops->mmap(&fds[device], &vma) == -EINVAL, "nor past a BAR's end");
}

/** The power state, which the owner writes and VFIO carries out. */
static void power_rows(void)
{
    struct fn *m = &fns[1];
    struct vfio_device *d = vdev("0000:00:02.0");
    uint64_t pmcsr = (7ull << 40) + 0xD4;
    uintptr_t phys = 0;
    uint16_t w = 0x0003;
    uint32_t v;
    unsigned int was = slept;
    int z = zaps;

    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 3 && zaps == z + 1 &&
              slept == was + PCI_PM_D3HOT_WAIT,
          "a write of D3hot puts the function there after Linux's wait, the BARs taken from "
          "the mappings");
    CHECK(pread_at(device, pmcsr, &w, 2) == 2 && (w & 3) == 3, "the owner reads D3hot back");
    CHECK(mapped_ops->page(d, (2u << 28) + 1, &phys) == -1 && pread_at(device, 0, &v, 4) == -EIO,
          "a memory BAR is not reached in D3hot");
    w = 0;
    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 0 &&
              get32(m->cfg + 0x18) == 0xE0000004u && (m->cfg[4] & 0x06) == 0x06,
          "back in D0, a function without No_Soft_Reset has its state written back");
    CHECK(mapped_ops->page(d, (2u << 28) + 1, &phys) == 0 && phys == 0xE0001000u &&
              pread_at(device, 0, &v, 4) == 4,
          "and its BARs are reached again");
    m->cfg[0xD4] = 0x0B; /* No_Soft_Reset, in D3hot behind VFIO's back */
    put32(m->cfg + 0x18, 0x00000004u);
    d->pm_restore = 0;
    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 0 &&
              get32(m->cfg + 0x18) == 0xE0000004u,
          "D0 with no state kept writes back the BARs the bus's walk found");
    m->cfg[0xD4] = 0;
    d->pm_restore = 1;
}

/** Power states checked: D1 and D2 only when the function offers them and
 * only deeper, D2 with Linux's wait both ways. Ends in D3hot. */
static void power_check_rows(void)
{
    struct fn *m = &fns[1];
    struct vfio_device *d = vdev("0000:00:02.0");
    uint64_t pmcsr = (7ull << 40) + 0xD4;
    uintptr_t phys = 0;
    uint16_t w = PCI_D2;
    uint32_t v;
    unsigned int us = udelayed;

    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 0 && udelayed == us,
          "D2 is refused by a function that does not offer it");
    put16(m->cfg + 0xD2, 0x0600); /* D1 and D2 offered */
    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 2 &&
              udelayed == us + PCI_PM_D2_DELAY && !d->power_low && pread_at(device, 0, &v, 4) == 4,
          "D2 is entered after Linux's 200 us, its BARs still reached");
    w = PCI_D1;
    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 2,
          "D1 is refused from the deeper D2");
    w = PCI_D0;
    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 0 &&
              udelayed == us + 2 * PCI_PM_D2_DELAY,
          "D0 from D2 waits the same 200 us");
    w = PCI_D3HOT;
    (void)pwrite_at(device, pmcsr, &w, 2);
    w = PCI_D1;
    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 3 && d->power_low &&
              mapped_ops->page(d, (2u << 28) + 1, &phys) == -1 &&
              pread_at(device, 0, &v, 4) == -EIO,
          "D1 is refused from D3hot, whose BARs stay out of reach");
    put16(m->cfg + 0xD2, 0);
}

/** A function that refuses a power change, in D3hot at the start: the state
 * read back decides the BARs' access and the state kept. */
static void power_refused_rows(void)
{
    struct fn *m = &fns[1];
    struct vfio_device *d = vdev("0000:00:02.0");
    uint64_t pmcsr = (7ull << 40) + 0xD4;
    uint16_t w = PCI_D0;
    uint32_t v;

    power_refused = 1;
    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 3 && d->power_low &&
              d->pm_saved_ok && pread_at(device, 0, &v, 4) == -EIO,
          "a function that refuses D0 keeps its BARs out of reach and its state owed");
    power_refused = 0;
    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 0 && !d->power_low &&
              !d->pm_saved_ok && get32(m->cfg + 0x18) == 0xE0000004u,
          "and has it written back once it takes D0");
    power_refused = 1;
    w = PCI_D3HOT;
    CHECK(pwrite_at(device, pmcsr, &w, 2) == 2 && (m->cfg[0xD4] & 3) == 0 && !d->power_low &&
              !d->pm_saved_ok && pread_at(device, 0, &v, 4) == 4,
          "a function that refuses D3hot keeps its BARs in reach and owes no state");
    power_refused = 0;
}

static int power_fd[2];
static int32_t power_result[2];

/** Request D0 through this call's own device description. */
static void write_d0(void)
{
    int id = fiber_running();
    uint16_t d0 = PCI_D0;

    power_result[id] = pwrite_at(power_fd[id], (7ull << 40) + 0xD4, &d0, sizeof(d0));
}

/** A second caller waits for the first D0 write to finish restoring state. */
static void power_wait_rows(void)
{
    struct vfio_device *d = vdev("0000:00:02.0");
    uint16_t d3 = PCI_D3HOT;
    uint64_t pmcsr = (7ull << 40) + 0xD4;

    power_fd[0] = device;
    power_fd[1] = ioc(group0, VFIO_GROUP_GET_DEVICE_FD, (uintptr_t)d->name);
    CHECK(power_fd[1] > 0, "another description of the same function opens");
    if (power_fd[1] <= 0) {
        return;
    }
    CHECK(pwrite_at(device, pmcsr, &d3, 2) == 2, "the function enters D3hot");
    pause_power_wait = 1;
    power_result[0] = power_result[1] = -1;
    CHECK(fiber_start(0, write_d0) == 0 && d->power_low && d->pm_saved_ok,
          "the first write waits before restoring the function");
    CHECK(fiber_start(1, write_d0) == 0 && power_result[1] == -1 && d->power_low,
          "another D0 write waits while the first is restoring the function");
    if (fiber_live(0)) {
        CHECK(fiber_resume(0) == 1, "the first write finishes after its wait");
    }
    CHECK(power_result[0] == 2 && !d->power_low && !d->pm_saved_ok &&
              get32(fns[1].cfg + 0x18) == 0xE0000004u,
          "the first write makes the restored function available");
    if (fiber_live(1)) {
        CHECK(fiber_resume(1) == 1, "the second write finishes after the restore");
    }
    CHECK(power_result[1] == 2, "the second write succeeds");
    pause_power_wait = 0;
    closefd(power_fd[1]);
}

/* ---- interrupts ---- */

/** VFIO_DEVICE_GET_IRQ_INFO. */
static void info_rows(void)
{
    struct vfio_irq_info ii = {sizeof(ii), 0, VFIO_PCI_INTX_IRQ_INDEX, 0};

    CHECK(ioc(device, VFIO_DEVICE_GET_IRQ_INFO, (uintptr_t)&ii) == 0 && ii.count == 1 &&
              ii.flags ==
                  (VFIO_IRQ_INFO_EVENTFD | VFIO_IRQ_INFO_MASKABLE | VFIO_IRQ_INFO_AUTOMASKED),
          "the line is maskable and masked at each interrupt");
    ii.index = VFIO_PCI_MSI_IRQ_INDEX;
    CHECK(ioc(device, VFIO_DEVICE_GET_IRQ_INFO, (uintptr_t)&ii) == 0 && ii.count == 2 &&
              ii.flags == (VFIO_IRQ_INFO_EVENTFD | VFIO_IRQ_INFO_NORESIZE),
          "MSI's two messages");
    ii.index = VFIO_PCI_MSIX_IRQ_INDEX;
    CHECK(ioc(device, VFIO_DEVICE_GET_IRQ_INFO, (uintptr_t)&ii) == 0 && ii.count == 0 &&
              ii.flags == VFIO_IRQ_INFO_EVENTFD,
          "no MSI-X, which may grow");
    ii.index = VFIO_PCI_ERR_IRQ_INDEX;
    CHECK(ioc(device, VFIO_DEVICE_GET_IRQ_INFO, (uintptr_t)&ii) == 0 && ii.count == 0,
          "no error eventfd without PCI Express");
    ii.index = VFIO_PCI_REQ_IRQ_INDEX;
    CHECK(ioc(device, VFIO_DEVICE_GET_IRQ_INFO, (uintptr_t)&ii) == 0 && ii.count == 1,
          "a request eventfd");
    ii.index = 5;
    CHECK(ioc(device, VFIO_DEVICE_GET_IRQ_INFO, (uintptr_t)&ii) == -EINVAL, "no sixth kind");
}

/* A SET_IRQS with room for four eventfds. */
struct set {
    struct vfio_irq_set h;
    int32_t fd[4];
};

/** SET_IRQS on `fd` with the header's fields and up to four values. */
static int32_t set_irqs(int fd, uint32_t flags, uint32_t index, uint32_t start, uint32_t count,
                        const int32_t *v)
{
    struct set s;
    uint32_t i;

    k_memset(&s, 0, sizeof(s));
    s.h.argsz = sizeof(s);
    s.h.flags = flags;
    s.h.index = index;
    s.h.start = start;
    s.h.count = count;
    for (i = 0; i < count && i < 4 && v != NULL; i++) {
        s.fd[i] = v[i];
    }
    if (flags & VFIO_IRQ_SET_DATA_BOOL) {
        k_memset(s.fd, 0, sizeof(s.fd));
        for (i = 0; i < count && i < 4 && v != NULL; i++) {
            ((uint8_t *)s.fd)[i] = (uint8_t)v[i];
        }
    }
    return ioc(fd, VFIO_DEVICE_SET_IRQS, (uintptr_t)&s);
}

#define EVENTFD_TRIGGER (VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER)
#define NONE_TRIGGER    (VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER)

/** The owner's Interrupt Disable over an interrupt it has not served, as
 * Linux's virq_disabled: no signal while it is set, and the mask kept until
 * it is clear. The line is unmasked and idle at the start and the end. */
static void intx_owner_rows(void)
{
    struct fn *m = &fns[1];
    uint64_t cmd = (7ull << 40) + 4;
    uint16_t w = 0x0406;

    m->cfg[6] |= 0x08;
    raise(IRQ_VECTOR(11));
    CHECK(evs[0].count == 6 && (m->cfg[5] & 0x04), "an interrupt the owner has not served");
    CHECK(pwrite_at(device, cmd, &w, 2) == 2 && (m->cfg[5] & 0x04) && evs[0].count == 6,
          "the owner's Interrupt Disable keeps the line masked");
    eventfd_signal(&evs[1]);
    vfio_irq_work();
    CHECK((m->cfg[5] & 0x04) && evs[0].count == 6,
          "an unmask while it is set keeps the mask and signals nothing");
    CHECK(set_irqs(device, NONE_TRIGGER, 0, 0, 1, NULL) == 0 && evs[0].count == 6,
          "nor does a trigger without data");
    w = 0x0006;
    CHECK(pwrite_at(device, cmd, &w, 2) == 2 && (m->cfg[5] & 0x04) && evs[0].count == 7,
          "clearing it while the line is asserted keeps the mask and signals again");
    m->cfg[6] &= (uint8_t)~0x08;
    eventfd_signal(&evs[1]);
    vfio_irq_work();
    CHECK((m->cfg[5] & 0x04) == 0, "and the owner's unmask then lifts the mask");
    w = 0x0406;
    (void)pwrite_at(device, cmd, &w, 2);
    w = 0x0006;
    CHECK(pwrite_at(device, cmd, &w, 2) == 2 && (m->cfg[5] & 0x04) == 0 && evs[0].count == 7,
          "setting and clearing it over an idle line unmasks it again without a signal");
}

/** The line: its eventfd, its mask at each interrupt, its unmasks. */
static void intx_rows(void)
{
    int32_t fd = 100, un = 1101;
    struct fn *m = &fns[1];
    uint16_t w = 0x0006;
    int wakes = irq_wakes;

    (void)pwrite_at(device, (7ull << 40) + 4, &w, 2); /* the owner's INTx on */
    CHECK(set_irqs(device, EVENTFD_TRIGGER, 5, 0, 1, &fd) == -EINVAL, "no sixth kind");
    CHECK(set_irqs(device, EVENTFD_TRIGGER, 0, 1, 1, &fd) == -EINVAL, "start past the count");
    CHECK(set_irqs(device, EVENTFD_TRIGGER | VFIO_IRQ_SET_DATA_BOOL, 0, 0, 1, &fd) == -EINVAL,
          "two kinds of data");
    CHECK(
        set_irqs(device,
                 VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_MASK | VFIO_IRQ_SET_ACTION_TRIGGER,
                 0, 0, 1, &fd) == -ENOTTY,
        "two actions");
    CHECK(set_irqs(device, EVENTFD_TRIGGER, 0, 0, 1, &fd) == 0 && evs[0].refs == 1 &&
              handlers[IRQ_VECTOR(11)] != NULL,
          "the line's eventfd, with VFIO's handler on line 11");
    CHECK((m->cfg[5] & 0x04) == 0, "Interrupt Disable lifted for the owner's line");
    raise(IRQ_VECTOR(11));
    CHECK(evs[0].count == 0, "a line the device does not assert is not its");
    m->cfg[6] |= 0x08;
    raise(IRQ_VECTOR(11));
    CHECK(evs[0].count == 1 && (m->cfg[5] & 0x04), "an interrupt masks the line and signals");
    raise(IRQ_VECTOR(11));
    CHECK(evs[0].count == 1, "a masked line signals no more");
    CHECK(set_irqs(device, VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK, 0, 0, 1, NULL) ==
                  0 &&
              evs[0].count == 2 && (m->cfg[5] & 0x04),
          "an unmask while the line is asserted stays masked and signals again");
    m->cfg[6] &= (uint8_t)~0x08;
    CHECK(set_irqs(device, VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK, 0, 0, 1, NULL) ==
                  0 &&
              (m->cfg[5] & 0x04) == 0,
          "an unmask lifts the mask");
    CHECK(set_irqs(device, VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_MASK, 0, 0, 1, &fd) ==
              -ENOTTY,
          "masking by eventfd is ENOTTY, as on Linux");
    CHECK(set_irqs(device, VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_UNMASK, 0, 0, 1, &un) ==
                  0 &&
              evs[1].refs == 1,
          "an unmask eventfd, its descriptor above a byte read whole");
    m->cfg[6] |= 0x08;
    raise(IRQ_VECTOR(11));
    eventfd_signal(&evs[1]);
    eventfd_signal(&evs[1]);
    CHECK((m->cfg[5] & 0x04) && evs[0].count == 3 && evs[1].count == 0 && vfio_irq_pending(NULL) &&
              irq_wakes == wakes + 1,
          "an asserted line's unmask requests coalesce before delivery");
    vfio_irq_work();
    CHECK((m->cfg[5] & 0x04) && evs[0].count == 4 && evs[1].count == 0,
          "the worker keeps the asserted line masked and signals again");
    m->cfg[6] &= (uint8_t)~0x08;
    eventfd_signal(&evs[1]);
    vfio_irq_work();
    CHECK((m->cfg[5] & 0x04) == 0 && evs[1].count == 0, "the worker unmasks the idle line");
    CHECK(set_irqs(device, NONE_TRIGGER, 0, 0, 1, NULL) == 0 && evs[0].count == 5,
          "a trigger without data signals the eventfd");
    intx_owner_rows();
    CHECK(set_irqs(device, EVENTFD_TRIGGER, VFIO_PCI_MSI_IRQ_INDEX, 0, 1, &fd) == -EINVAL,
          "MSI while the line is on is refused");
    eventfd_signal(&evs[1]);
    CHECK(vfio_irq_pending(NULL), "an unmask is queued before the line is turned off");
    CHECK(set_irqs(device, NONE_TRIGGER, 0, 0, 0, NULL) == 0 && evs[0].refs == 0 &&
              evs[1].refs == 0 && (m->cfg[5] & 0x04),
          "no data and no count turns the line off");
    CHECK(!vfio_irq_pending(NULL), "turning the line off clears pending work");
}

/** MSI, and VFIO_DEVICE_RESET with it on. */
static void msi_rows(void)
{
    int32_t fd[4] = {102, 103, -1, 104};
    uint64_t pmcsr = (7ull << 40) + 0xD4;
    uint16_t d3 = PCI_D3HOT, d0 = PCI_D0;
    int v;

    CHECK(set_irqs(device, EVENTFD_TRIGGER, VFIO_PCI_MSI_IRQ_INDEX, 0, 2, fd) == 1,
          "two messages are answered with the one there is");
    CHECK(set_irqs(device, EVENTFD_TRIGGER, VFIO_PCI_MSI_IRQ_INDEX, 0, 1, fd) == 0 &&
              msi_vector >= 48 && vectors_held() == 1,
          "MSI takes a vector");
    v = msi_vector;
    raise(v);
    CHECK(evs[2].count == 1, "a message signals its eventfd");
    CHECK(ioc(device, VFIO_DEVICE_RESET, 0) == 0 && fns[1].resets == 2 && msi_enables == 2 &&
              msi_vector == v,
          "a reset programs the message again");
    CHECK(pwrite_at(device, pmcsr, &d3, 2) == 2 && pwrite_at(device, pmcsr, &d0, 2) == 2 &&
              msi_vector == v && msi_enables == 3 && vectors_held() == 1,
          "D0 restores the message after a D3hot soft reset");
    if (msi_vector == v) {
        raise(msi_vector);
    }
    CHECK(evs[2].count == 2, "the restored message reaches its original eventfd");
    fd[0] = -1;
    if (msi_vector >= 48) {
        in_flight[msi_vector] = 1;
    }
    CHECK(set_irqs(device, EVENTFD_TRIGGER, VFIO_PCI_MSI_IRQ_INDEX, 0, 1, fd) == 0 &&
              msi_vector == -1 && vectors_held() == 0 && evs[2].refs == 0,
          "removing the MSI eventfd turns off the message before giving the vector back");
    CHECK(evs[2].count == 3 && freed_in_flight == 0,
          "after a message sent before then has signalled its eventfd");
    CHECK(set_irqs(device, NONE_TRIGGER, VFIO_PCI_MSI_IRQ_INDEX, 0, 0, NULL) == 0 &&
              vectors_held() == 0 && evs[2].refs == 0 && msi_vector == -1,
          "MSI off gives its vector back");
}

/** MSI-X messages that return on the xHCI after a D3hot soft reset. */
static void msix_power_rows(int xhci)
{
    uint64_t pmcsr = (7ull << 40) + 0xD4;
    uint16_t d3 = PCI_D3HOT, d0 = PCI_D0;
    int v1 = msix_vectors[1], v3 = msix_vectors[3], v6 = msix_vectors[6];

    CHECK(pwrite_at(xhci, pmcsr, &d3, 2) == 2 && pwrite_at(xhci, pmcsr, &d0, 2) == 2 &&
              msix_vectors[1] == v1 && msix_vectors[3] == v3 && msix_vectors[6] == v6 &&
              msix_vectors[0] == -1 && vectors_held() == 3,
          "D0 restores the active MSI-X vectors");
    CHECK(get32(fns[2].mem + 0x2000 + 12) == 1 && get32(fns[2].mem + 0x2000 + 16 + 12) == 0,
          "D0 masks unused MSI-X entries and unmasks active ones");
    if (v3 >= 48 && msix_vectors[3] == v3) {
        raise(v3);
    }
    CHECK(evs[4].count == 2, "the restored MSI-X entry reaches its original eventfd");
}

/** An I/O BAR the firmware left without an address, BAR 4 of the xHCI,
 * which would decode ports 0 to 255: neither exposed nor decoded. */
static void unassigned_io_rows(int xhci)
{
    struct vfio_region_info ri;
    uint8_t b[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    uint16_t w = 0x0007;
    int32_t rd, wr;
    int i, untouched = 1;

    ri.argsz = sizeof(ri);
    ri.index = 4;
    CHECK(ioc(xhci, VFIO_DEVICE_GET_REGION_INFO, (uintptr_t)&ri) == 0 && ri.size == 0 &&
              ri.flags == 0,
          "an unassigned I/O BAR is no region");
    rd = pread_at(xhci, (4ull << 40) + 0x20, b, 1);
    wr = pwrite_at(xhci, (4ull << 40) + 0x40, b, 4);
    CHECK(rd == -EINVAL && wr == -EINVAL, "and reads and writes no port");
    for (i = 0; i < 256; i++) {
        untouched = untouched && ports[i] == 0;
    }
    CHECK(untouched, "the host's ports 0 to 255 are not reached");
    CHECK((fns[2].cfg[4] & 0x01) == 0, "the open turns its I/O decoding off");
    CHECK(pwrite_at(xhci, (7ull << 40) + 4, &w, 2) == 2 && (fns[2].cfg[4] & 0x07) == 0x06,
          "and the owner cannot turn it on");
}

/** MSI-X refused, with nothing mapped, for a table outside a claimed
 * memory BAR: in BAR 3, which the xHCI lacks, and past BAR 0's end. */
static void msix_table_rows(int g2)
{
    static const uint32_t table[2] = {0x2003u, 0xFFC0u};
    static const char *what[2] = {"MSI-X with its table in a BAR the function lacks is refused",
                                  "MSI-X with its table past its BAR's end is refused"};
    int32_t fd[1] = {102};
    int xhci, k, maps;

    for (k = 0; k < 2; k++) {
        put32(fns[2].cfg + 0x84, table[k]);
        xhci = ioc(g2, VFIO_GROUP_GET_DEVICE_FD, (uintptr_t)"0000:00:14.0");
        maps = bar_maps;
        CHECK(xhci > 0 &&
                  set_irqs(xhci, EVENTFD_TRIGGER, VFIO_PCI_MSIX_IRQ_INDEX, 0, 1, fd) == -EINVAL &&
                  bar_maps == maps && vectors_held() == 0,
              what[k]);
        if (xhci > 0) {
            closefd(xhci);
        }
    }
    put32(fns[2].cfg + 0x84, 0x2000u);
}

/** MSI-X on the xHCI, entry by entry. */
static void msix_rows(void)
{
    int32_t fd[4] = {102, 103, -1, 104};
    int32_t g2fd = container;
    int xhci, v, g2;

    g2 = open_node("vfio/2");
    CHECK(ioc(g2, VFIO_GROUP_SET_CONTAINER, (uintptr_t)&g2fd) == 0, "the xHCI's group");
    add_bar(&fns[2], 2, 0, 0x1000, 0, 0); /* memory the firmware left unassigned */
    fns[2].cfg[4] &= (uint8_t)~0x02u;
    CHECK(ioc(g2, VFIO_GROUP_GET_DEVICE_FD, (uintptr_t)"0000:00:14.0") == -EINVAL &&
              (fns[2].cfg[4] & 0x02) == 0 && fns[2].resets == 0,
          "a memory BAR without an address refuses the open, with no reset and memory "
          "decoding off");
    add_bar(&fns[2], 2, 0, 0, 0, 0);
    add_bar(&fns[2], 4, 0, 256, 1, 0); /* ports the firmware left unassigned */
    fns[2].cfg[4] |= 0x01;
    xhci = ioc(g2, VFIO_GROUP_GET_DEVICE_FD, (uintptr_t)"0000:00:14.0");
    CHECK(xhci > 0, "the xHCI opens");
    unassigned_io_rows(xhci);
    CHECK(set_irqs(xhci, EVENTFD_TRIGGER, VFIO_PCI_MSIX_IRQ_INDEX, 0, 4, fd) == 0 &&
              msix_vectors[0] >= 48 && msix_vectors[1] >= 48 && msix_vectors[2] == -1 &&
              msix_vectors[3] >= 48 && msix_unmasks == 1 && windows == 1,
          "MSI-X programs each entry given an eventfd, with the table mapped");
    raise(msix_vectors[3]);
    CHECK(evs[4].count == 1, "an entry's message signals its eventfd");
    fd[0] = -1;
    CHECK(set_irqs(xhci, EVENTFD_TRIGGER, VFIO_PCI_MSIX_IRQ_INDEX, 0, 1, fd) == 0 &&
              msix_vectors[0] == -1 && vectors_held() == 2,
          "an entry given -1 is masked and its vector given back");
    fd[0] = 105;
    CHECK(set_irqs(xhci, EVENTFD_TRIGGER, VFIO_PCI_MSIX_IRQ_INDEX, 6, 1, fd) == 0 &&
              msix_vectors[6] >= 48,
          "an entry is added while MSI-X is on");
    msix_power_rows(xhci);
    CHECK(pread_at(xhci, 0x2000, &v, 4) == 4 && v == -1, "the table reads as all ones");
    v = 0x12345678;
    CHECK(pwrite_at(xhci, 0x2000, &v, 4) == 4 && get32(fns[2].mem + 0x2000) == 0,
          "and takes no write");
    fns[2].mem[0x10] = 0xAB;
    CHECK(pread_at(xhci, 0x10, &v, 1) == 1 && (v & 0xFF) == 0xAB, "the rest of BAR 0 reads");
    closefd(xhci);
    CHECK(vectors_held() == 0 && evs[3].refs == 0 && evs[4].refs == 0 && evs[5].refs == 0 &&
              windows == 0 && fns[2].resets == 2,
          "the release gives back every vector, eventfd and window, and resets");
    msix_table_rows(g2);
    closefd(g2);
}

/** The release after the last descriptor and mapping, and the group
 * and container after it. */
static void release_rows(void)
{
    struct vfio_iommu_type1_dma_map map = {sizeof(map), 3, 0x10000, 0x100000, 0x4000};
    struct vfio_device *d = vdev("0000:00:02.0");
    int32_t fd = 1106;
    uint16_t d3 = 0x0003; /* the power state D3hot */

    CHECK(set_irqs(device, VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER,
                   VFIO_PCI_REQ_IRQ_INDEX, 0, 1, &fd) == 0 &&
              evs[6].refs == 1,
          "the request eventfd is kept, its descriptor above a byte read whole");
    fns[1].cfg[0x0C] = 0x77;
    CHECK(pwrite_at(device, (7ull << 40) + 0xD4, &d3, 2) == 2 && (fns[1].cfg[0xD4] & 3) == 3,
          "the owner leaves the function in D3hot");
    closefd(device);
    CHECK(d->refs == 1 && fns[1].resets == 2, "the mapping holds the device past its close");
    CHECK(ioc(group0, VFIO_GROUP_UNSET_CONTAINER, 0) == -EBUSY,
          "the group stays in its container while the device is held");
    mapped_ops->put(d);
    CHECK(d->refs == 0 && fns[1].resets == 3 && fns[1].cfg[0x0C] == 0 && evs[6].refs == 0,
          "the last reference resets the device and puts its state back");
    CHECK((fns[1].cfg[0xD4] & 3) == 0 && get32(fns[1].cfg + 0x18) == 0xE0000004u,
          "in D0 first, as Linux's vfio_pci_core_disable");
    reaps = 0;
    pin_hook = unset_last_group;
    CHECK(ioc(container, VFIO_IOMMU_MAP_DMA, (uintptr_t)&map) == -EINVAL && unset_r == 0 &&
              pins == 0,
          "a MAP_DMA whose pin slept past the last group's going out fails and unpins");
    CHECK(detaches == 3 && domains == 0, "the last group out releases the driver and its domain");
    CHECK(unset_reaps == 1 && reaps == 2 && reap_pins == 0,
          "the detach and the failed map each release the pins' files at their end");
    closefd(group0);
    reaps = 0;
    closefd(container);
    CHECK(reaps == 1, "the container's close releases the pins' files at its end");
    CHECK(slept != 0, "the resets waited through VFIO's sleep");
}

int main(void)
{
    build();
    group_rows();
    binding_rows();
    container_rows();
    iommu_info_rows();
    dma_rows();
    dma_race_rows();
    region_rows();
    config_rows();
    pcie_rows();
    map_rows();
    power_rows();
    power_check_rows();
    power_refused_rows();
    power_wait_rows();
    info_rows();
    intx_rows();
    msi_rows();
    msix_rows();
    release_rows();
    printf("vfio-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
