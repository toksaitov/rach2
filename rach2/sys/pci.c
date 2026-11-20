/*
 * pci.c: the devices on the bus, found by walking configuration space, their
 * base address registers and capabilities, and the messages a device sends
 * in place of an interrupt line.
 *
 * How configuration space and the bus's I/O space are reached, and what a
 * message is written to, are the architecture's (arch/x86/pciconf.c on a
 * PC); the rest is the bus's own, the same on every machine.
 */

#include "arch/arch.h"
#include "klog.h"
#include "pci.h"
#include "kio.h"
#include "proc/percpu.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "lib/kstring.h"
#include "lib/spinlock.h"
#include "mm/kheap.h"
#include "mm/paging.h"

/*
 * The owners of functions: the driver that has each, so that a function
 * goes to one at a time. A driver of the kernel is recorded as it first
 * turns on a function's decoding, bus mastering, line or messages through
 * this file (`pci_claim_kernel`); VFIO records itself when a function is
 * given to vfio-pci. A function with no record has no driver.
 */
#define PCI_OWNERS_MAX 64

static struct pci_owner_rec {
    uint16_t key; /* bus << 8 | device << 3 | function */
    const char *owner;
} pci_owners[PCI_OWNERS_MAX];

/** Whether the owners' names `a` and `b` are the same text. */
static int owner_same(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The record of the function `key`, or NULL. */
static struct pci_owner_rec *owner_find(uint16_t key)
{
    int i;

    for (i = 0; i < PCI_OWNERS_MAX; i++) {
        if (pci_owners[i].owner != NULL && pci_owners[i].key == key) {
            return &pci_owners[i];
        }
    }
    return NULL;
}

int pci_claim(uint8_t bus, uint8_t devfn, const char *owner)
{
    uint16_t key = (uint16_t)(bus << 8 | devfn);
    struct pci_owner_rec *r = owner_find(key);
    int i;

    if (r != NULL) {
        return owner_same(r->owner, owner) ? 0 : -EBUSY;
    }
    for (i = 0; i < PCI_OWNERS_MAX; i++) {
        if (pci_owners[i].owner == NULL) {
            pci_owners[i].key = key;
            pci_owners[i].owner = owner;
            return 0;
        }
    }
    return -ENOSPC;
}

void pci_unclaim(uint8_t bus, uint8_t devfn, const char *owner)
{
    struct pci_owner_rec *r = owner_find((uint16_t)(bus << 8 | devfn));

    if (r != NULL && owner_same(r->owner, owner)) {
        r->owner = NULL;
    }
}

const char *pci_owner(uint8_t bus, uint8_t devfn)
{
    struct pci_owner_rec *r = owner_find((uint16_t)(bus << 8 | devfn));

    return r != NULL ? r->owner : NULL;
}

/** Record the kernel as `dev`'s owner, unless it has one; a function given
 * to VFIO stays VFIO's, since VFIO enables its messages through here. */
static void pci_claim_kernel(const pci_device_t *dev)
{
    uint8_t devfn = (uint8_t)(dev->device << 3 | dev->function);

    if (pci_owner(dev->bus, devfn) == NULL) {
        (void)pci_claim(dev->bus, devfn, PCI_OWNER_KERNEL);
    }
}

/** The word is taken from its aligned dword, the half that bit 1 of the
 * offset selects. */
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t val = pci_read_config32(bus, device, function, offset & ~3);
    return (val >> ((offset & 2) * 8)) & 0xFFFF;
}

/** The byte is taken from its aligned dword, the byte that the low two bits
 * of the offset select. */
uint8_t pci_read_config8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    uint32_t val = pci_read_config32(bus, device, function, offset & ~3);
    return (val >> ((offset & 3) * 8)) & 0xFF;
}

/**
 * Fill `out` with the place, identity, class, interrupt line register and
 * six BARs of a function: nine dword reads of configuration space.
 */
static void pci_read_device_info(uint8_t bus, uint8_t device, uint8_t function, pci_device_t *out)
{
    uint32_t id, cls;
    int i;

    id = pci_read_config32(bus, device, function, PCI_VENDOR_ID);
    /* The dword at 0x08: revision, prog-if, subclass, class. */
    cls = pci_read_config32(bus, device, function, PCI_REVISION_ID);
    out->bus = bus;
    out->device = device;
    out->function = function;
    out->vendor_id = (uint16_t)(id & 0xFFFF);
    out->device_id = (uint16_t)(id >> 16);
    out->revision = (uint8_t)cls;
    out->prog_if = (uint8_t)(cls >> 8);
    out->subclass = (uint8_t)(cls >> 16);
    out->class_code = (uint8_t)(cls >> 24);
    out->irq = (uint8_t)pci_read_config32(bus, device, function, PCI_INTERRUPT_LINE);

    /* A type 0 header has six BARs at offsets PCI_BAR0, +4, ... +0x14. */
    for (i = 0; i < 6; i++) {
        out->bar[i] = pci_read_config32(bus, device, function, PCI_BAR0 + i * 4);
    }
}

/*
 * The functions on the buses, found by one walk the first time a search
 * needs them and kept for every later search, so that a search reads
 * configuration space only for the function it returns. Under a hypervisor
 * each configuration access is an exit, and a walk of all 8192 slots of 256
 * buses costs some 16,000 of them; the walk reads bus 0 and the bus behind
 * each bridge it finds there and below, as Linux's does, and so reads only
 * the buses that exist (`pci_list_build` has the other root buses). The
 * list is kept in the order of bus, device and function.
 *
 * The list holds each function's place, identity, class and header type,
 * and the size of each BAR and of its ROM, which the walk finds as Linux's
 * enumeration does (`pci_list_size`); what may change after the walk (the
 * BARs' addresses, the interrupt line, a bridge's bus numbers) is read
 * again when a search returns it. The first search is
 * the page cache's window probe, early in the boot and before a second
 * processor runs, so the walk needs no lock. After it a function is added
 * or removed only for a hot plug (`pci_slot_add`, `pci_slot_remove`),
 * under the big lock, which every later search holds.
 *
 * A single-function device may answer for all eight function numbers as
 * function 0, so functions 1 to 7 are read only when function 0's header
 * type has bit 7 set, which marks a multi-function device. A vendor ID of
 * 0xFFFF, or of 0x0000 from a bridge that answers an empty slot with zeros,
 * is no function, as Linux takes it.
 */
struct pci_place {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t header; /* the header type byte, bit 7 masked off */
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint64_t size[PCI_RESOURCES]; /* bytes each BAR and the ROM decode; 0 for none */
};

/* The list starts in `pci_first` and moves to the heap, twice as long each
 * time, when a machine has more functions; `pci_list_len` is -1 before the
 * walk. */
#define PCI_FIRST_MAX 64
static struct pci_place pci_first[PCI_FIRST_MAX];
static struct pci_place *pci_list = pci_first;
static int pci_list_cap = PCI_FIRST_MAX;
static int pci_list_len = -1;

/** True when a function's identity dword says nothing answered. */
static bool pci_id_absent(uint32_t id)
{
    return (id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0;
}

/**
 * The decode size of a BAR from what it read back with all ones written,
 * `sz`, under the bits `mask` that may hold an address, as Linux's
 * pci_size: the lowest such bit set, or 0 when none is.
 */
static uint64_t pci_decode_size(uint64_t sz, uint64_t mask)
{
    sz &= mask;
    return sz & ~(sz - 1);
}

/**
 * Write `mask` over the register at `off` of a function, read what stays,
 * and put its value back, as Linux's __pci_read_base: what was read.
 */
static uint32_t pci_size_reg(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t mask)
{
    uint32_t v = pci_read_config32(bus, dev, fn, off), sz;

    pci_write_config32(bus, dev, fn, off, v | mask);
    sz = pci_read_config32(bus, dev, fn, off);
    pci_write_config32(bus, dev, fn, off, v);
    return sz;
}

/** A low dword or ROM register's read-back as Linux takes it: all ones,
 * from a function that does not answer, as 0. */
static uint32_t pci_answered(uint32_t sz)
{
    return sz == 0xFFFFFFFFu ? 0 : sz;
}

/**
 * Size the BARs and the ROM of the function `p` lists, as Linux's
 * pci_read_bases does at enumeration: six BARs and the ROM at 0x30 for a
 * header of type 0, two and the ROM at 0x38 for a bridge, one for a CardBus
 * bridge. Memory and I/O decoding are off meanwhile, so that no access
 * lands at an address half written, but for a host bridge, which Linux
 * keeps decoding (quirk_mmio_always_on). It runs in the walk, before any
 * driver has started the device.
 */
static void pci_list_size(struct pci_place *p)
{
    int bars = p->header == 0 ? 6 : p->header == 1 ? 2 : p->header == 2 ? 1 : 0;
    uint8_t rom = p->header == 0 ? 0x30 : p->header == 1 ? 0x38 : 0;
    uint8_t off;
    uint16_t cmd = 0;
    uint32_t v, lo;
    uint64_t sz;
    int i, quiet = !(p->class_code == 0x06 && p->subclass == 0x00);

    for (i = 0; i < PCI_RESOURCES; i++) {
        p->size[i] = 0;
    }
    if (quiet) {
        cmd = pci_read_config16(p->bus, p->device, p->function, PCI_COMMAND);
        if (cmd & (PCI_CMD_IO_SPACE | PCI_CMD_MEMORY_SPACE)) {
            pci_write_config16(p->bus, p->device, p->function, PCI_COMMAND,
                               (uint16_t)(cmd & ~(PCI_CMD_IO_SPACE | PCI_CMD_MEMORY_SPACE)));
        }
    }
    for (i = 0; i < bars; i++) {
        off = (uint8_t)(PCI_BAR0 + 4 * i);
        v = pci_read_config32(p->bus, p->device, p->function, off);
        lo = pci_answered(pci_size_reg(p->bus, p->device, p->function, off, 0xFFFFFFFFu));
        if (v & 0x01) {
            p->size[i] = pci_decode_size(lo, 0xFFFCu);
        } else if (pci_bar_is_64bit(v) && i + 1 < bars) {
            sz = (uint64_t)pci_size_reg(p->bus, p->device, p->function, (uint8_t)(off + 4),
                                        0xFFFFFFFFu)
                     << 32 |
                 lo;
            p->size[i++] = pci_decode_size(sz, 0xFFFFFFFFFFFFFFF0ULL);
        } else {
            p->size[i] = pci_decode_size(lo, 0xFFFFFFF0u);
        }
    }
    if (rom) {
        p->size[PCI_ROM_RESOURCE] = pci_decode_size(
            pci_answered(pci_size_reg(p->bus, p->device, p->function, rom, 0xFFFFF800u)),
            0xFFFFF800u);
    }
    if (quiet && (cmd & (PCI_CMD_IO_SPACE | PCI_CMD_MEMORY_SPACE))) {
        pci_write_config16(p->bus, p->device, p->function, PCI_COMMAND, cmd);
    }
}

/**
 * Append a function to the list, reading its class and sizing its BARs;
 * false, with the reason in the log, when the list is full and no longer
 * one can be had.
 */
static bool pci_list_add(uint8_t bus, uint8_t device, uint8_t function, uint32_t id, uint8_t header)
{
    struct pci_place *p;
    uint32_t cls;

    if (pci_list_len == pci_list_cap) {
        struct pci_place *bigger = kmalloc((size_t)pci_list_cap * 2 * sizeof(*bigger));

        if (bigger == NULL) {
            klog(KLOG_WARN, "pci", "list: full at %d functions: no memory for more", pci_list_cap);
            return false;
        }
        k_memcpy(bigger, pci_list, (size_t)pci_list_len * sizeof(*bigger));
        if (pci_list != pci_first) {
            kfree(pci_list);
        }
        pci_list = bigger;
        pci_list_cap *= 2;
    }
    cls = pci_read_config32(bus, device, function, PCI_REVISION_ID);
    p = &pci_list[pci_list_len++];
    p->bus = bus;
    p->device = device;
    p->function = function;
    p->header = (uint8_t)(header & 0x7F);
    p->vendor_id = (uint16_t)(id & 0xFFFF);
    p->device_id = (uint16_t)(id >> 16);
    p->subclass = (uint8_t)(cls >> 16);
    p->class_code = (uint8_t)(cls >> 24);
    pci_list_size(p);
    return true;
}

/* A bit for each bus number. */
typedef uint32_t pci_busset[8];

#define PCI_BUS_IN(set, b)  (((set)[(b) >> 5] >> ((b) & 31)) & 1u)
#define PCI_BUS_ADD(set, b) ((set)[(b) >> 5] |= 1u << ((b) & 31))

/**
 * List the functions of `bus`, and add to `reached` the secondary bus of
 * each PCI-to-PCI or CardBus bridge among them; false when the list could
 * not grow.
 */
static bool pci_list_bus(uint8_t bus, pci_busset reached)
{
    uint8_t device, function, functions, header, header0, secondary;
    uint32_t id, id0;

    for (device = 0; device < 32; device++) {
        id0 = pci_read_config32(bus, device, 0, PCI_VENDOR_ID);
        if (pci_id_absent(id0)) {
            continue;
        }
        header0 = pci_read_config8(bus, device, 0, PCI_HEADER_TYPE);
        functions = (header0 & 0x80) ? 8 : 1;
        for (function = 0; function < functions; function++) {
            id = function == 0 ? id0 : pci_read_config32(bus, device, function, PCI_VENDOR_ID);
            if (pci_id_absent(id)) {
                continue;
            }
            header =
                function == 0 ? header0 : pci_read_config8(bus, device, function, PCI_HEADER_TYPE);
            if (!pci_list_add(bus, device, function, id, header)) {
                return false;
            }
            if ((header & 0x7F) != 0x01 && (header & 0x7F) != 0x02) {
                continue;
            }
            /* Both bridge headers keep the secondary bus at 0x19; a
             * bridge firmware left unnumbered has 0 there. */
            secondary = pci_read_config8(bus, device, function, 0x19);
            if (secondary != 0) {
                PCI_BUS_ADD(reached, secondary);
            }
        }
    }
    return true;
}

/** Sort the list by bus, device and function: a second pass of the walk
 * appends the buses that firmware numbered below their bridge's own. */
static void pci_list_sort(void)
{
    int i, j;

    for (i = 1; i < pci_list_len; i++) {
        struct pci_place t = pci_list[i];
        uint32_t key = (uint32_t)t.bus << 8 | (uint32_t)t.device << 3 | t.function;

        for (j = i; j > 0; j--) {
            const struct pci_place *q = &pci_list[j - 1];

            if (((uint32_t)q->bus << 8 | (uint32_t)q->device << 3 | q->function) <= key) {
                break;
            }
            pci_list[j] = pci_list[j - 1];
        }
        pci_list[j] = t;
    }
}

/**
 * Walk bus 0 and every bus behind a bridge, in rising order of bus, once
 * each, and fill the list. A bus numbered below the bridge that leads to it
 * is read by another pass, so the walk ends with a pass that reads no bus;
 * each pass reads only buses not read before, and there are 256.
 *
 * A machine with several host bridges has a root bus for each, which no
 * bridge on bus 0 leads to and which Linux finds through ACPI. Once the
 * bridges are followed, device 0 of the next bus not yet read is read, and
 * a bus where it answers is walked as a root and its bridges followed before
 * the next bus is read: the host bridge or root port of such a bus is its
 * device 0 on the AMD and Intel parts that have them. Device 0 of each bus
 * is read at most once, 255 reads.
 */
static void pci_list_build(void)
{
    pci_busset reached, walked;
    uint16_t bus;
    uint16_t probe = 1; /* the next bus whose device 0 is to be read */
    bool more;
    int i;

    for (i = 0; i < 8; i++) {
        reached[i] = 0;
        walked[i] = 0;
    }
    PCI_BUS_ADD(reached, 0);
    pci_list_len = 0;
    do {
        more = false;
        for (bus = 0; bus < 256; bus++) {
            if (!PCI_BUS_IN(reached, bus) || PCI_BUS_IN(walked, bus)) {
                continue;
            }
            PCI_BUS_ADD(walked, bus);
            more = true;
            if (!pci_list_bus((uint8_t)bus, reached)) {
                pci_list_sort();
                return;
            }
        }
        for (; !more && probe < 256; probe++) {
            if (!PCI_BUS_IN(reached, probe) &&
                !pci_id_absent(pci_read_config32((uint8_t)probe, 0, 0, PCI_VENDOR_ID))) {
                PCI_BUS_ADD(reached, probe);
                more = true;
                klog(KLOG_INFO, "pci", "list: bus %d is a root of its own", (int)probe);
            }
        }
    } while (more);
    pci_list_sort();
    klog(KLOG_DEBUG, "pci", "list: %d functions", pci_list_len);
}

/** The number of functions on the list, walking the buses on the first
 * call. */
static int pci_list_count(void)
{
    if (pci_list_len < 0) {
        pci_list_build();
    }
    return pci_list_len;
}

/** Fill `out` from the list's entry `i`, reading its registers now. */
static void pci_list_take(int i, pci_device_t *out)
{
    pci_read_device_info(pci_list[i].bus, pci_list[i].device, pci_list[i].function, out);
}

bool pci_find_device(uint8_t class_code, uint8_t subclass, pci_device_t *out)
{
    return pci_find_device_skip(class_code, subclass, NULL, out);
}

/** The search matches from the entry after `skip_dev`'s, and finds nothing
 * when `skip_dev` is not on the list. */
bool pci_find_device_skip(uint8_t class_code, uint8_t subclass, pci_device_t *skip_dev,
                          pci_device_t *out)
{
    int i, n = pci_list_count();
    bool past_skip = (skip_dev == NULL);

    for (i = 0; i < n; i++) {
        const struct pci_place *p = &pci_list[i];

        if (!past_skip) {
            if (p->bus == skip_dev->bus && p->device == skip_dev->device &&
                p->function == skip_dev->function) {
                past_skip = true;
            }
            continue;
        }
        if (p->class_code == class_code && p->subclass == subclass) {
            pci_list_take(i, out);
            return true;
        }
    }
    return false;
}

bool pci_find_device_by_id(uint16_t vendor_id, uint16_t device_id, pci_device_t *out)
{
    return pci_find_nth_by_id(vendor_id, device_id, 0, out);
}

bool pci_nth(int nth, pci_device_t *out)
{
    if (nth < 0 || nth >= pci_list_count()) {
        return false;
    }
    pci_list_take(nth, out);
    return true;
}

void pci_resource(uint8_t bus, uint8_t device, uint8_t function, int i, struct pci_resource *r)
{
    const struct pci_place *p = NULL;
    uint8_t off;
    uint32_t v;
    int n = pci_list_count(), k;

    r->start = 0;
    r->end = 0;
    r->flags = 0;
    for (k = 0; k < n; k++) {
        if (pci_list[k].bus == bus && pci_list[k].device == device &&
            pci_list[k].function == function) {
            p = &pci_list[k];
            break;
        }
    }
    if (p == NULL || i < 0 || i >= PCI_RESOURCES || p->size[i] == 0) {
        return;
    }
    if (i == PCI_ROM_RESOURCE) {
        v = pci_read_config32(bus, device, function, p->header == 1 ? 0x38 : 0x30);
        r->start = v & 0xFFFFF800u;
        r->flags = (v & 0x01u) | PCI_IORESOURCE_MEM | PCI_IORESOURCE_PREFETCH |
                   PCI_IORESOURCE_READONLY | PCI_IORESOURCE_SIZEALIGN;
    } else {
        off = (uint8_t)(PCI_BAR0 + 4 * i);
        v = pci_read_config32(bus, device, function, off);
        if (v & 0x01u) {
            r->start = v & 0xFFFFFFFCu;
            r->flags = (v & 0x03u) | PCI_IORESOURCE_IO;
        } else {
            r->start = v & 0xFFFFFFF0u;
            r->flags = (v & 0x0Fu) | PCI_IORESOURCE_MEM;
            if (v & 0x08u) {
                r->flags |= PCI_IORESOURCE_PREFETCH;
            }
            if (pci_bar_is_64bit(v)) {
                r->start |= (uint64_t)pci_read_config32(bus, device, function, (uint8_t)(off + 4))
                            << 32;
                r->flags |= PCI_IORESOURCE_MEM_64;
            }
        }
        r->flags |= PCI_IORESOURCE_SIZEALIGN;
    }
    r->end = r->start + p->size[i] - 1;
}

bool pci_find_nth_by_id(uint16_t vendor_id, uint16_t device_id, int nth, pci_device_t *out)
{
    int i, n = pci_list_count(), seen = 0;

    for (i = 0; i < n; i++) {
        if (pci_list[i].vendor_id == vendor_id && pci_list[i].device_id == device_id &&
            seen++ == nth) {
            pci_list_take(i, out);
            return true;
        }
    }
    return false;
}

/** The list's index of `bus`:`device`.`function`, or -1. */
static int pci_list_find(uint8_t bus, uint8_t device, uint8_t function)
{
    int i, n = pci_list_count();

    for (i = 0; i < n; i++) {
        if (pci_list[i].bus == bus && pci_list[i].device == device &&
            pci_list[i].function == function) {
            return i;
        }
    }
    return -1;
}

int pci_slot_add(uint8_t bus, uint8_t device)
{
    uint8_t function, functions, header;
    uint32_t id;
    int added = 0;

    (void)pci_list_count();
    if (pci_id_absent(pci_read_config32(bus, device, 0, PCI_VENDOR_ID))) {
        return 0;
    }
    functions = (pci_read_config8(bus, device, 0, PCI_HEADER_TYPE) & 0x80) ? 8 : 1;
    for (function = 0; function < functions; function++) {
        id = pci_read_config32(bus, device, function, PCI_VENDOR_ID);
        if (pci_id_absent(id) || pci_list_find(bus, device, function) >= 0) {
            continue;
        }
        header = pci_read_config8(bus, device, function, PCI_HEADER_TYPE);
        if (!pci_list_add(bus, device, function, id, header)) {
            break;
        }
        added++;
    }
    if (added > 0) {
        pci_list_sort();
    }
    return added;
}

int pci_slot_remove(uint8_t bus, uint8_t device)
{
    int i, kept = 0, n = pci_list_count();

    for (i = 0; i < n; i++) {
        if (pci_list[i].bus != bus || pci_list[i].device != device) {
            pci_list[kept++] = pci_list[i];
        }
    }
    pci_list_len = kept;
    return n - kept;
}

/**
 * Window `w` of the PCI-to-PCI bridge `p`, as its base and limit registers
 * give it: 0 its I/O, 1 its memory, 2 its prefetchable memory, into
 * `*start` and `*end`. 0 for a window the bridge leaves closed, whose base
 * is above its limit.
 */
static int pci_bridge_window(const struct pci_place *p, int w, uint64_t *start, uint64_t *end)
{
    uint32_t lo;

    if (w == 0) {
        lo = pci_read_config32(p->bus, p->device, p->function, 0x1C);
        *start = (uint64_t)(lo & 0xF0u) << 8;
        *end = (uint64_t)(lo & 0xF000u) | 0xFFFu;
        if ((lo & 0x0Fu) == 0x01u) {
            lo = pci_read_config32(p->bus, p->device, p->function, 0x30);
            *start |= (uint64_t)(lo & 0xFFFFu) << 16;
            *end |= (uint64_t)(lo >> 16) << 16;
        }
    } else {
        lo = pci_read_config32(p->bus, p->device, p->function, w == 1 ? 0x20 : 0x24);
        *start = (uint64_t)(lo & 0xFFF0u) << 16;
        *end = (uint64_t)(lo & 0xFFF00000u) | 0xFFFFFu;
        if (w == 2 && (lo & 0x0Fu) == 0x01u) {
            *start |= (uint64_t)pci_read_config32(p->bus, p->device, p->function, 0x28) << 32;
            *end |= (uint64_t)pci_read_config32(p->bus, p->device, p->function, 0x2C) << 32;
        }
    }
    return *start <= *end;
}

/* The PC's fixed I/O ports, which Linux on x86 reserves before it claims a
 * PCI resource (standard_io_resources, the RTC's and "PCI conf1"), and the
 * ports of the devices the kernel drives at fixed addresses, which Linux's
 * drivers request: the two legacy ATA channels ata.c probes and COM1, the
 * serial console. */
static const struct {
    uint16_t start, end;
} pci_pc_ports[] = {{0x00, 0x1F},   {0x20, 0x21},   {0x40, 0x43},   {0x50, 0x53},   {0x60, 0x60},
                    {0x64, 0x64},   {0x70, 0x77},   {0x80, 0x8F},   {0xA0, 0xA1},   {0xC0, 0xDF},
                    {0xF0, 0xFF},   {0x170, 0x177}, {0x1F0, 0x1F7}, {0x376, 0x376}, {0x3F6, 0x3F6},
                    {0x3F8, 0x3FF}, {0xCF8, 0xCFF}};

/* The host's other ranges, which `pci_host_reserve` was given: a PC's map
 * and system devices give a few dozen, merged where they touch. */
#define PCI_HOST_RANGES 64

static struct pci_aperture pci_host_ranges[PCI_HOST_RANGES];
static int pci_host_nranges;

/* The windows of bus 0's host bridge (`pci_root_windows`). */
#define PCI_ROOT_WINDOWS 8

static struct pci_aperture pci_root_win[PCI_ROOT_WINDOWS];
static int pci_root_nwin;

/** Whether `a` to `b` and `c` to `d`, inclusive, overlap or adjoin. */
static int pci_ranges_touch(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return !(b < c && c - b > 1u) && !(d < a && a - d > 1u);
}

/** Make room in a full table: the two ranges of one kind with the least
 * between them become one, which holds what lay between them too. A full
 * table of two kinds always has two of one; 0 when it has not. */
static int pci_host_squeeze(void)
{
    struct pci_aperture *a, *b;
    uint64_t gap, best = 0;
    int i, j, bi = -1, bj = -1;

    for (i = 0; i < pci_host_nranges; i++) {
        for (j = i + 1; j < pci_host_nranges; j++) {
            a = &pci_host_ranges[i];
            b = &pci_host_ranges[j];
            if (!a->io != !b->io) {
                continue;
            }
            gap =
                a->end < b->start ? b->start - a->end : (b->end < a->start ? a->start - b->end : 0);
            if (bi < 0 || gap < best) {
                bi = i;
                bj = j;
                best = gap;
            }
        }
    }
    if (bi < 0 || bj < 0) {
        return 0;
    }
    a = &pci_host_ranges[bi];
    b = &pci_host_ranges[bj];
    a->start = a->start < b->start ? a->start : b->start;
    a->end = a->end > b->end ? a->end : b->end;
    *b = pci_host_ranges[--pci_host_nranges];
    return 1;
}

void pci_host_reserve(uint64_t start, uint64_t end, int io)
{
    struct pci_aperture *h;
    int k;

    if (end < start) {
        return;
    }
    for (k = 0; k < pci_host_nranges;) {
        h = &pci_host_ranges[k];
        if (!h->io != !io || !pci_ranges_touch(start, end, h->start, h->end)) {
            k++;
            continue;
        }
        start = h->start < start ? h->start : start;
        end = h->end > end ? h->end : end;
        *h = pci_host_ranges[--pci_host_nranges];
        k = 0;
    }
    if (pci_host_nranges == PCI_HOST_RANGES && !pci_host_squeeze()) {
        return;
    }
    h = &pci_host_ranges[pci_host_nranges++];
    h->start = start;
    h->end = end;
    h->io = io != 0;
}

void pci_root_windows(const struct pci_aperture *ap, int n)
{
    int k;

    pci_root_nwin = 0;
    for (k = 0; k < n && pci_root_nwin < PCI_ROOT_WINDOWS; k++) {
        pci_root_win[pci_root_nwin++] = ap[k];
    }
}

/** Whether `start` to `end`, of I/O ports when `io` is set, overlaps a
 * range of the host's own: a fixed port or one `pci_host_reserve` holds;
 * `*next` is then the address after it. */
static int pci_host_taken(uint64_t start, uint64_t end, int io, uint64_t *next)
{
    int j;

    for (j = 0; io && j < (int)(sizeof(pci_pc_ports) / sizeof(pci_pc_ports[0])); j++) {
        if (start <= pci_pc_ports[j].end && pci_pc_ports[j].start <= end) {
            *next = (uint64_t)pci_pc_ports[j].end + 1u;
            return 1;
        }
    }
    for (j = 0; j < pci_host_nranges; j++) {
        const struct pci_aperture *h = &pci_host_ranges[j];

        if (!h->io == !io && start <= h->end && h->start <= end) {
            *next = h->end + 1u;
            return 1;
        }
    }
    return 0;
}

/** Whether `start` to `end`, of I/O ports when `io` is set, overlaps a
 * window of a listed bridge; `*next` is then the address after it. */
static int pci_window_taken(uint64_t start, uint64_t end, int io, uint64_t *next)
{
    uint64_t ws, we;
    int k, w, n = pci_list_count();

    for (k = 0; k < n; k++) {
        for (w = io ? 0 : 1; pci_list[k].header == 1 && w < (io ? 1 : 3); w++) {
            if (pci_bridge_window(&pci_list[k], w, &ws, &we) && we >= start && ws <= end) {
                *next = we + 1;
                return 1;
            }
        }
    }
    return 0;
}

/** Whether `start` to `end`, of I/O ports when `io` is set, overlaps a BAR
 * with an address of a listed function or a bridge's window; `*next` is
 * then the address after what it overlaps. */
static int pci_range_taken(uint64_t start, uint64_t end, int io, uint64_t *next)
{
    struct pci_resource r;
    int i, k, n = pci_list_count();

    if (pci_window_taken(start, end, io, next) || pci_host_taken(start, end, io, next)) {
        return 1;
    }
    for (k = 0; k < n; k++) {
        for (i = 0; i < PCI_RESOURCES; i++) {
            pci_resource(pci_list[k].bus, pci_list[k].device, pci_list[k].function, i, &r);
            if (r.start == 0 || !(r.flags & (io ? PCI_IORESOURCE_IO : PCI_IORESOURCE_MEM)) ||
                r.end < start || r.start > end) {
                continue;
            }
            *next = r.end + 1;
            return 1;
        }
    }
    return 0;
}

/** Whether bus 0's host bridge passes `r`: a window of its kind holds it
 * whole, or the firmware gave the bridge no window of that kind. */
static int pci_root_holds(const struct pci_resource *r)
{
    int k, any = 0, io = (r->flags & PCI_IORESOURCE_IO) != 0;

    for (k = 0; k < pci_root_nwin; k++) {
        if (!pci_root_win[k].io != !io) {
            continue;
        }
        any = 1;
        if (pci_root_win[k].start <= r->start && r->end <= pci_root_win[k].end) {
            return 1;
        }
    }
    return !any;
}

/**
 * Whether the host reaches `r`, a resource of a function on `bus`, as
 * Linux's pci_find_parent_resource finds a parent at every level up to the
 * root: each bridge on the way that decodes positively has a window of the
 * kind of `r` that holds it whole, a prefetchable window only for a
 * prefetchable BAR; a subtractive one passes it to its own bus; and at bus
 * 0 the host bridge's windows hold it (`pci_root_holds`). Another root
 * bus, whose host bridge's windows are not kept, passes it. A bridge
 * without a bus number leads nowhere, and a loop of bridges reaches
 * nothing.
 */
static int pci_route_holds(uint8_t bus, const struct pci_resource *r)
{
    uint64_t ws, we;
    uint8_t sec;
    int depth, k, w, held, n = pci_list_count();
    int io = (r->flags & PCI_IORESOURCE_IO) != 0;
    int last = io ? 1 : ((r->flags & PCI_IORESOURCE_PREFETCH) ? 3 : 2);

    for (depth = 0; depth < 256; depth++) {
        const struct pci_place *p = NULL;

        for (k = 0; k < n && p == NULL; k++) {
            if (pci_list[k].header == 1) {
                sec = pci_read_config8(pci_list[k].bus, pci_list[k].device, pci_list[k].function,
                                       0x19);
                if (sec != 0 && sec == bus) {
                    p = &pci_list[k];
                }
            }
        }
        if (p == NULL) {
            return bus != 0 || pci_root_holds(r);
        }
        if (pci_read_config8(p->bus, p->device, p->function, PCI_PROG_IF) != 0x01) {
            held = 0;
            for (w = io ? 0 : 1; w < last && !held; w++) {
                held = pci_bridge_window(p, w, &ws, &we) && ws <= r->start && r->end <= we;
            }
            if (!held) {
                return 0;
            }
        }
        bus = p->bus;
    }
    return 0;
}

/** Whether `r`, of the function bus:device.function, overlaps a window of
 * another bridge on its bus, which Linux's request_resource finds as a
 * sibling. */
static int pci_sibling_window(uint8_t bus, uint8_t device, uint8_t function,
                              const struct pci_resource *r)
{
    uint64_t ws, we;
    int k, w, n = pci_list_count();
    int io = (r->flags & PCI_IORESOURCE_IO) != 0;

    for (k = 0; k < n; k++) {
        const struct pci_place *p = &pci_list[k];

        if (p->header != 1 || p->bus != bus || (p->device == device && p->function == function)) {
            continue;
        }
        for (w = io ? 0 : 1; w < (io ? 1 : 3); w++) {
            if (pci_bridge_window(p, w, &ws, &we) && ws <= r->end && r->start <= we) {
                return 1;
            }
        }
    }
    return 0;
}

int pci_resource_claimable(uint8_t bus, uint8_t device, uint8_t function, int i)
{
    struct pci_resource r, o;
    uint64_t next;
    uint32_t kind;
    int j, k, n = pci_list_count();

    pci_resource(bus, device, function, i, &r);
    kind = r.flags & (PCI_IORESOURCE_IO | PCI_IORESOURCE_MEM);
    if (i < 0 || i >= PCI_ROM_RESOURCE || kind == 0 || r.start == 0 || r.end < r.start ||
        (kind == PCI_IORESOURCE_IO && r.end > 0xFFFFu) || !pci_route_holds(bus, &r)) {
        return -EINVAL;
    }
    if (pci_host_taken(r.start, r.end, kind == PCI_IORESOURCE_IO, &next) ||
        pci_sibling_window(bus, device, function, &r)) {
        return -EBUSY;
    }
    for (k = 0; k < n; k++) {
        const struct pci_place *p = &pci_list[k];

        for (j = 0; j < PCI_ROM_RESOURCE; j++) {
            if (p->bus == bus && p->device == device && p->function == function && j == i) {
                continue;
            }
            pci_resource(p->bus, p->device, p->function, j, &o);
            if ((o.flags & kind) && o.start != 0 && o.start <= r.end && r.start <= o.end) {
                return -EBUSY;
            }
        }
    }
    return 0;
}

/**
 * The lowest address in the apertures `ap` for `size` bytes aligned to
 * `size`, of I/O when `io` is set, below `limit`, that no listed function
 * decodes; 0 for none.
 */
static uint64_t pci_place_bar(const struct pci_aperture *ap, int n, uint64_t size, int io,
                              uint64_t limit)
{
    uint64_t at, next;
    int a;

    for (a = 0; a < n; a++) {
        if (!ap[a].io != !io) {
            continue;
        }
        at = ap[a].start;
        if (io && at < PCI_IO_FLOOR) {
            at = PCI_IO_FLOOR;
        }
        for (;;) {
            at = (at + size - 1) & ~(size - 1);
            if (at == 0 || at + size - 1 > ap[a].end || at + size - 1 > limit) {
                break;
            }
            if (!pci_range_taken(at, at + size - 1, io, &next)) {
                return at;
            }
            at = next;
        }
    }
    return 0;
}

int pci_assign_bars(uint8_t bus, uint8_t device, uint8_t function, const struct pci_aperture *ap,
                    int n)
{
    int k = pci_list_find(bus, device, function), i, rc = 0;
    struct pci_resource r;
    uint64_t size, at;
    uint8_t off;
    uint32_t v;

    if (k < 0) {
        return -ENODEV;
    }
    for (i = 0; i < PCI_ROM_RESOURCE; i++) {
        size = pci_list[k].size[i];
        pci_resource(bus, device, function, i, &r);
        if (size == 0 || r.start != 0) {
            continue;
        }
        off = (uint8_t)(PCI_BAR0 + 4 * i);
        v = pci_read_config32(bus, device, function, off);
        at = pci_place_bar(ap, n, size, (int)(v & 0x01u),
                           (v & 0x01u)           ? 0xFFFFu
                           : pci_bar_is_64bit(v) ? ~(uint64_t)0
                                                 : 0xFFFFFFFFu);
        if (at == 0) {
            klog(KLOG_WARN, "pci", "assign: %02x:%02x.%d: BAR %d of %u bytes fits nowhere", bus,
                 device, function, i, (unsigned)size);
            rc = -ENOSPC;
            continue;
        }
        pci_write_config32(bus, device, function, off,
                           (uint32_t)at | (v & ((v & 0x01u) ? 0x03u : 0x0Fu)));
        if (!(v & 0x01u) && pci_bar_is_64bit(v)) {
            pci_write_config32(bus, device, function, (uint8_t)(off + 4), (uint32_t)(at >> 32));
            i++;
        }
    }
    return rc;
}

void pci_enable_bus_master(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    pci_claim_kernel(dev);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/**
 * The write that actually stops a device writing into memory. A driver that
 * turns its receiver and transmitter off has told the device to stop; this
 * takes away its ability to do it anyway, which is what matters when the
 * memory it holds addresses for is about to belong to something else.
 */
void pci_disable_bus_master(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    cmd &= (uint16_t)~PCI_CMD_BUS_MASTER;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

void pci_enable_memory_space(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    pci_claim_kernel(dev);
    cmd |= PCI_CMD_MEMORY_SPACE;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/** The Interrupt Disable bit masks the line while it is set. */
void pci_enable_interrupts(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    pci_claim_kernel(dev);
    cmd &= ~PCI_CMD_INTERRUPT_DIS;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/** An I/O BAR has its flags in bits 1:0 (mask 0xFFFFFFFC), a memory BAR in
 * bits 3:0 (mask 0xFFFFFFF0). */
uint32_t pci_bar_address(uint32_t bar)
{
    if (bar & 0x01) {
        return bar & 0xFFFFFFFC;
    } else {
        return bar & 0xFFFFFFF0;
    }
}

bool pci_bar_is_memory(uint32_t bar)
{
    return !(bar & 0x01);
}

/** A memory BAR's type, bits 2:1, is 0b10 (0x04 in place) for a 64-bit
 * BAR. */
bool pci_bar_is_64bit(uint32_t bar)
{
    return pci_bar_is_memory(bar) && ((bar & 0x06) == 0x04);
}

uint64_t pci_bar_address64(const pci_device_t *dev, int i)
{
    uint64_t phys;

    if (i < 0 || i > 5 || !pci_bar_is_memory(dev->bar[i])) {
        return 0;
    }
    phys = pci_bar_address(dev->bar[i]);
    if (pci_bar_is_64bit(dev->bar[i])) {
        if (i == 5) {
            return 0;
        }
        phys |= (uint64_t)dev->bar[i + 1] << 32;
    }
    return phys;
}

/** The BAR's mask is read with the command register's memory decoding off
 * and the BAR's value restored before decoding is put back as it was. A
 * 64-bit BAR's upper half is sized with it, as one 64-bit mask. */
uint64_t pci_bar_size64(const pci_device_t *dev, int i)
{
    uint8_t off = (uint8_t)(PCI_BAR0 + 4 * i);
    uint16_t cmd;
    uint32_t lo, hi = 0xFFFFFFFFu;
    uint64_t mask;
    int wide;

    if (i < 0 || i > 5 || !pci_bar_is_memory(dev->bar[i])) {
        return 0;
    }
    wide = pci_bar_is_64bit(dev->bar[i]);
    if (wide && i == 5) {
        return 0;
    }
    cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND,
                       (uint16_t)(cmd & ~PCI_CMD_MEMORY_SPACE));
    pci_write_config32(dev->bus, dev->device, dev->function, off, 0xFFFFFFFFu);
    lo = pci_read_config32(dev->bus, dev->device, dev->function, off);
    pci_write_config32(dev->bus, dev->device, dev->function, off, dev->bar[i]);
    if (wide) {
        pci_write_config32(dev->bus, dev->device, dev->function, (uint8_t)(off + 4), 0xFFFFFFFFu);
        hi = pci_read_config32(dev->bus, dev->device, dev->function, (uint8_t)(off + 4));
        pci_write_config32(dev->bus, dev->device, dev->function, (uint8_t)(off + 4),
                           dev->bar[i + 1]);
    }
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
    mask = ((uint64_t)hi << 32) | (lo & 0xFFFFFFF0u);
    if (!wide) {
        mask |= 0xFFFFFFFF00000000ULL;
    }
    return (lo & 0xFFFFFFF0u) == 0 && !wide ? 0 : ~mask + 1;
}

uintptr_t pci_bar_map(const pci_device_t *dev, int i, uint64_t offset, size_t size, pte_t cache,
                      struct pci_window *w)
{
    uint64_t base = pci_bar_address64(dev, i);
    uint64_t bar_size = w->bar_size;

    w->va = 0;
    if (base == 0 || size == 0 || offset + size < offset ||
        (bar_size != 0 && (offset >= bar_size || size > bar_size - offset))) {
        return 0;
    }
    w->va = mmio_map(base + offset, size, cache);
    w->phys = base + offset;
    w->size = size;
    return w->va;
}

void pci_bar_unmap(struct pci_window *w)
{
    if (w->va != 0) {
        mmio_unmap(w->va, w->size);
        w->va = 0;
    }
}

/**
 * The list is read only when the status register says one exists, and the
 * walk is bounded twice over: a fixed hop count, and an end at any offset
 * outside the capability region. A device that answers its own pointer with
 * garbage therefore ends the search, not the machine.
 */
uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t cap_id)
{
    uint16_t status = pci_read_config16(bus, device, function, PCI_STATUS);
    uint8_t next;
    int hops;

    if (!(status & 0x0010)) {
        return 0; /* the device declares no capability list */
    }
    next = pci_read_config8(bus, device, function, PCI_CAP_PTR);
    for (hops = 0; hops < 48; hops++) {
        uint8_t offset, id;

        /* All ones is an absent device's; the mask would turn it into 0xFC. */
        if (next == 0xFF) {
            return 0;
        }
        offset = (uint8_t)(next & 0xFC);
        if (offset < 0x40) {
            return 0;
        }
        id = pci_read_config8(bus, device, function, offset);
        if (id == cap_id) {
            return offset;
        }
        next = pci_read_config8(bus, device, function, (uint8_t)(offset + 1));
    }
    return 0;
}

/** Header type 1 is a bridge's, and its secondary bus number is the byte at
 * offset 0x19, read now, since firmware or a driver may number the buses
 * after the walk. */
bool pci_find_bridge_for_bus(uint8_t bus, pci_device_t *out)
{
    int i, n = pci_list_count();

    for (i = 0; i < n; i++) {
        const struct pci_place *p = &pci_list[i];

        if (p->header != 0x01) {
            continue; /* not a bridge */
        }
        if (pci_read_config8(p->bus, p->device, p->function, 0x19) != bus) {
            continue;
        }
        out->bus = p->bus;
        out->device = p->device;
        out->function = p->function;
        out->vendor_id = p->vendor_id;
        out->device_id = p->device_id;
        return true;
    }
    return false;
}

/*
 * The messages enabled through this file, kept so that an interrupt
 * remapper that starts or stops later can rewrite each one: a device's MSI,
 * and each entry of its MSI-X table programmed here. Each holds the
 * compatibility-format message `pci_msi_message` made and the remapper's
 * entry, if any; the device has the remapped message while the entry is
 * held.
 */
#define MSI_RECORDS   256
#define MSI_KIND_MSI  1
#define MSI_KIND_MSIX 2

static struct msi_record {
    pci_device_t dev;
    uint8_t kind;     /* 0 for a free record, MSI_KIND_MSI or MSI_KIND_MSIX */
    uint8_t cap;      /* the MSI capability's offset */
    uint16_t index;   /* the MSI-X entry's index; 0 for MSI */
    uint16_t control; /* its control word, for the place of the data */
    uintptr_t entry;  /* the MSI-X table entry, mapped */
    uint32_t address; /* the compatibility-format message */
    uint32_t data;
    int handle;           /* the remapper's entry, or -1 */
    int cpu;              /* logical destination, serialized with the device by msi_lock */
    uint32_t destination; /* the whole APIC id, also in x2APIC mode */
    uint64_t affinity;    /* requested mask; zero until first composition */
} msi_records[MSI_RECORDS];

/* The remapper in place, or NULL; it and the records are under the lock,
 * which is held across each device's writes so that a remapper starting on
 * another processor never meets a message half written. */
static const struct pci_msi_remap *msi_remap;
static struct spinlock msi_lock = SPINLOCK_INIT_NAMED("msi_lock");
/* Published before normal shutdown stops another processor; zero means
 * no PCI message, otherwise the destination's logical number plus one. */
static volatile unsigned int msi_stop_targets[256];
/* Processors observed online, retained if one later goes offline. */
static uint64_t msi_possible;

/** The record of `dev`'s message of `kind` at `index`, its MSI-X entry,
 * or NULL. */
static struct msi_record *msi_record_at(const pci_device_t *dev, int kind, unsigned int index)
{
    int i;

    for (i = 0; i < MSI_RECORDS; i++) {
        struct msi_record *r = &msi_records[i];

        if (r->kind == kind && r->index == index && r->dev.bus == dev->bus &&
            r->dev.device == dev->device && r->dev.function == dev->function) {
            return r;
        }
    }
    return NULL;
}

/** The record of `dev`'s message of `kind`, entry 0 of an MSI-X table. */
static struct msi_record *msi_record_find(const pci_device_t *dev, int kind)
{
    return msi_record_at(dev, kind, 0);
}

/** The record of `dev`'s message of `kind` at `index`, a free one taken
 * when it has none: NULL when every record is taken. */
static struct msi_record *msi_record_get_at(const pci_device_t *dev, int kind, unsigned int index)
{
    struct msi_record *r = msi_record_at(dev, kind, index);
    int i;

    for (i = 0; r == NULL && i < MSI_RECORDS; i++) {
        if (msi_records[i].kind == 0) {
            r = &msi_records[i];
            r->dev = *dev;
            r->kind = (uint8_t)kind;
            r->index = (uint16_t)index;
            r->handle = -1;
            r->affinity = 0;
            r->cpu = -1;
        }
    }
    if (r == NULL) {
        klog(KLOG_WARN, "pci", "msi: %x:%x.%d: every record of a message is taken", dev->bus,
             dev->device, dev->function);
    }
    return r;
}

/** The record of `dev`'s message of `kind`, entry 0 of an MSI-X table, a
 * free one taken when it has none. */
static struct msi_record *msi_record_get(const pci_device_t *dev, int kind)
{
    return msi_record_get_at(dev, kind, 0);
}

/**
 * Write the message (address, data) of record `r` to its device. Moving to
 * the remapped format, the address goes first, since it alone names the
 * remapper's entry and its data is not read; moving back, the data goes
 * first, so that each single write leaves a whole message of one format.
 */
static void msi_record_write(const struct msi_record *r, uint32_t address, uint32_t data,
                             int remapped)
{
    const pci_device_t *dev = &r->dev;

    if (r->kind == MSI_KIND_MSI) {
        uint8_t data_at = (uint8_t)(r->cap + ((r->control & PCI_MSI_64BIT) ? 12u : 8u));

        if (!remapped) {
            pci_write_config16(dev->bus, dev->device, dev->function, data_at, (uint16_t)data);
        }
        pci_write_config32(dev->bus, dev->device, dev->function, (uint8_t)(r->cap + 4), address);
        if (remapped) {
            pci_write_config16(dev->bus, dev->device, dev->function, data_at, (uint16_t)data);
        }
        return;
    }
    if (!remapped) {
        mmio_write32(r->entry + 8, data);
    }
    mmio_write32(r->entry, address);
    if (remapped) {
        mmio_write32(r->entry + 8, data);
    }
}

/** Free record `r`: a remapped message is written back in the compatibility
 * format and its entry given back. Called with the lock held. */
static void msi_record_drop(struct msi_record *r)
{
    if (r->handle >= 0 && msi_remap != NULL) {
        msi_record_write(r, r->address, r->data, 0);
        msi_remap->unmap(r->handle);
    }
    r->kind = 0;
    r->handle = -1;
}

/** Pick the online processor with the fewest allocated messages, with
 * Linux's matrix_find_best_cpu's first-processor tie break. The caller
 * holds msi_lock; `skip` is the message being moved, or NULL. */
static int msi_choose(uint64_t mask, const struct msi_record *skip)
{
    unsigned int load[NR_CPUS];
    unsigned int least = MSI_RECORDS + 1u;
    int c, i, best = -1;
    uint64_t online = msi_remap ? msi_remap->cpus() : pci_msi_cpus();

    k_memset(load, 0, sizeof(load));
    for (i = 0; i < MSI_RECORDS; i++) {
        const struct msi_record *r = &msi_records[i];

        if (r != skip && r->kind && r->cpu >= 0) {
            load[r->cpu]++;
        }
    }
    msi_possible |= online;
    mask &= online;
    for (c = 0; c < NR_CPUS; c++) {
        if ((mask & ((uint64_t)1 << c)) && load[c] < least) {
            least = load[c];
            best = c;
        }
    }
    return best;
}

/**
 * The message `dev` is to send for `vector`, in `*address` and `*data`: the
 * remapper's, with its entry in `*handle`, while one is in place, and the
 * compatibility format's otherwise, with `*handle` -1. The compatibility
 * format's message goes to `r`. 0, or -1, with `r` as it was, when the
 * remapper has no entry to give. Called with the lock held.
 */
static int msi_compose(struct msi_record *r, uint8_t vector, uint32_t *address, uint32_t *data,
                       int *handle)
{
    uint32_t was_address = r->address, was_data = r->data;

    if (!r->affinity) {
        r->affinity = ~(uint64_t)0;
        r->cpu = msi_choose(r->affinity, r);
    }
    if (r->cpu < 0) {
        return -1;
    }
    pci_msi_message_cpu(vector, r->cpu, &r->address, &r->data, &r->destination);
    *address = r->address;
    *data = r->data;
    *handle = -1;
    if (msi_remap != NULL) {
        *handle = msi_remap->map(&r->dev, r->address, r->data, r->destination, address, data);
        if (*handle < 0) {
            r->address = was_address;
            r->data = was_data;
            klog(KLOG_WARN, "pci", "msi: %x:%x.%d: no remapping entry", r->dev.bus, r->dev.device,
                 r->dev.function);
            return -1;
        }
    }
    return 0;
}

/** `pci_msi_disable` with the lock held. The enable bit is cleared before
 * a remapped message is written back and its entry given back, so that the
 * device sends nothing the remapper would refuse. */
static void msi_disable_locked(const pci_device_t *dev)
{
    uint8_t cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSI);
    struct msi_record *r = msi_record_find(dev, MSI_KIND_MSI);
    uint16_t control;

    if (cap != 0) {
        control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
        pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                           (uint16_t)(control & ~PCI_MSI_ENABLE));
    }
    if (r != NULL) {
        msi_record_drop(r);
    }
}

/** Disable MSI-X and drop every entry's message record, with msi_lock held. */
static void msix_disable_locked(const pci_device_t *dev);

/**
 * The order is the one the PCI specification requires and no other: the
 * enable bit is cleared first, in case firmware left it set, and the count
 * of vectors with it, so that nothing is sent while the address and the
 * data are written; an MSI-X capability is disabled, because a device with
 * both enabled is undefined; the address and the data are written; a device
 * that can mask its own vectors has this one's mask cleared, since firmware
 * may have left it set and the vector would then be programmed, enabled and
 * silent; and the enable bit is written last, when everything it acts on is
 * written. A count of zero tells a device that offers several vectors to use
 * one. A remapping entry the device held before is given back once the new
 * message is written.
 *
 * `last` is the last byte the writes reach: the data's, or with per-vector
 * masking the mask register's, past the data. A capability that would run
 * past the 256 bytes an offset can name fails the call, since the writes
 * would wrap around onto the identity registers at offset zero.
 */
int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    uint8_t cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSI);
    struct msi_record *r;
    uint16_t control;
    uint32_t address, data, flags;
    unsigned int last;
    int handle, old;

    if (cap == 0) {
        klog(KLOG_WARN, "pci", "msi: %x:%x.%d has no msi capability", dev->bus, dev->device,
             dev->function);
        return -1;
    }
    pci_claim_kernel(dev);
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));

    last = (unsigned int)cap + ((control & PCI_MSI_64BIT) ? 12u : 8u) + 1u;
    if ((control & PCI_MSI_PER_VECTOR) != 0) {
        last = (unsigned int)cap + ((control & PCI_MSI_64BIT) ? 16u : 12u) + 3u;
    }
    if (last > 0xFFu) {
        klog(KLOG_WARN, "pci", "msi: %x:%x.%d has a capability that runs past the end of its space",
             dev->bus, dev->device, dev->function);
        return -1;
    }

    flags = spin_lock_irqsave(&msi_lock);
    r = msi_record_get(dev, MSI_KIND_MSI);
    if (r == NULL) {
        spin_unlock_irqrestore(&msi_lock, flags);
        return -1;
    }
    old = r->handle;
    if (msi_compose(r, vector, &address, &data, &handle) != 0) {
        if (old < 0) {
            r->kind = 0;
        }
        spin_unlock_irqrestore(&msi_lock, flags);
        return -1;
    }
    control = (uint16_t)(control & ~(PCI_MSI_ENABLE | PCI_MSI_MULTIPLE_ENABLE));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2), control);
    msix_disable_locked(dev);
    pci_write_config32(dev->bus, dev->device, dev->function, (uint8_t)(cap + 4), address);
    if ((control & PCI_MSI_64BIT) != 0) {
        pci_write_config32(dev->bus, dev->device, dev->function, (uint8_t)(cap + 8), 0);
        pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 12),
                           (uint16_t)data);
    } else {
        pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 8),
                           (uint16_t)data);
    }
    if ((control & PCI_MSI_PER_VECTOR) != 0) {
        /* The mask register follows the data; its low bit is this vector. */
        uint8_t mask_at = (uint8_t)(cap + ((control & PCI_MSI_64BIT) ? 16u : 12u));

        pci_write_config32(dev->bus, dev->device, dev->function, mask_at, 0);
    }
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)(control | PCI_MSI_ENABLE));
    r->cap = cap;
    r->control = control;
    r->handle = handle;
    if (old >= 0) {
        msi_remap->unmap(old);
    }
    spin_unlock_irqrestore(&msi_lock, flags);
    klog(KLOG_DEBUG, "pci", "msi: %x:%x.%d on vector %d%s", dev->bus, dev->device, dev->function,
         vector, handle >= 0 ? ", remapped" : "");
    return 0;
}

/**
 * Find `dev`'s MSI-X capability: its offset, or 0 when it has none or one
 * whose twelve bytes would run past the end of configuration space.
 */
static uint8_t pci_msix_capability(const pci_device_t *dev)
{
    uint8_t cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSIX);

    return cap > 0xF4u ? 0 : cap;
}

int pci_msix_table(const pci_device_t *dev, uint8_t *bar, uint32_t *offset, uint32_t *entries)
{
    uint8_t cap = pci_msix_capability(dev);
    uint32_t where;
    uint16_t control;

    if (cap == 0) {
        return -1;
    }
    where = pci_read_config32(dev->bus, dev->device, dev->function, (uint8_t)(cap + 4));
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    *bar = (uint8_t)(where & PCI_MSIX_BIR);
    *offset = where & ~PCI_MSIX_BIR;
    *entries = (control & PCI_MSIX_TABLE_LEN) + 1u;
    return 0;
}

/** `pci_msix_hold` with the lock held. */
static int msix_hold_locked(const pci_device_t *dev)
{
    uint8_t cap = pci_msix_capability(dev);
    uint16_t control;

    if (cap == 0) {
        return -1;
    }
    pci_claim_kernel(dev);
    msi_disable_locked(dev);
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)(control | PCI_MSIX_ENABLE | PCI_MSIX_MASK_ALL));
    return 0;
}

/** MSI goes down first, since a device with both enabled is undefined. */
int pci_msix_hold(const pci_device_t *dev)
{
    uint32_t flags = spin_lock_irqsave(&msi_lock);
    int r = msix_hold_locked(dev);

    spin_unlock_irqrestore(&msi_lock, flags);
    return r;
}

/**
 * MSI-X is held first, enabled with every vector masked, because some
 * devices answer their table only while MSI-X is enabled; the entry is
 * written with the same address and data MSI sends, or the remapper's, and
 * its own mask cleared; and the function mask comes off last, when the
 * entry is whole.
 */
int pci_msix_enable(const pci_device_t *dev, uintptr_t table, uint8_t vector)
{
    uint8_t cap = pci_msix_capability(dev);
    struct msi_record *r;
    uint16_t control;
    uint32_t address, data, flags;
    int handle, old;

    flags = spin_lock_irqsave(&msi_lock);
    if (!table || msix_hold_locked(dev) != 0) {
        spin_unlock_irqrestore(&msi_lock, flags);
        klog(KLOG_WARN, "pci", "msi-x: %x:%x.%d has no usable msi-x capability", dev->bus,
             dev->device, dev->function);
        return -1;
    }
    r = msi_record_get(dev, MSI_KIND_MSIX);
    if (r == NULL) {
        spin_unlock_irqrestore(&msi_lock, flags);
        return -1;
    }
    old = r->handle;
    if (msi_compose(r, vector, &address, &data, &handle) != 0) {
        if (old < 0) {
            r->kind = 0;
        }
        spin_unlock_irqrestore(&msi_lock, flags);
        return -1;
    }
    r->entry = table;
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    mmio_write32(table, address);
    mmio_write32(table + 4, 0);
    mmio_write32(table + 8, data);
    mmio_write32(table + 12, 0);
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)((control | PCI_MSIX_ENABLE) & ~PCI_MSIX_MASK_ALL));
    r->handle = handle;
    if (old >= 0) {
        msi_remap->unmap(old);
    }
    spin_unlock_irqrestore(&msi_lock, flags);
    klog(KLOG_DEBUG, "pci", "msi-x: %x:%x.%d on vector %d%s", dev->bus, dev->device, dev->function,
         vector, handle >= 0 ? ", remapped" : "");
    return 0;
}

/**
 * The entry is masked by its own vector control bit while its address and
 * data are written, the address first, and unmasked after, so that the
 * device never sends half a message; a remapping entry the entry held
 * before is given back once the new message is written.
 */
int pci_msix_entry_set(const pci_device_t *dev, uintptr_t table, unsigned int index, uint8_t vector)
{
    uint8_t bar;
    uint32_t offset, entries, address, data, flags;
    uintptr_t entry = table + (uintptr_t)index * 16u;
    struct msi_record *r;
    int handle, old;

    if (!table || pci_msix_table(dev, &bar, &offset, &entries) != 0 || index >= entries) {
        return -1;
    }
    flags = spin_lock_irqsave(&msi_lock);
    r = msi_record_get_at(dev, MSI_KIND_MSIX, index);
    if (r == NULL) {
        spin_unlock_irqrestore(&msi_lock, flags);
        return -1;
    }
    old = r->handle;
    if (msi_compose(r, vector, &address, &data, &handle) != 0) {
        if (old < 0) {
            r->kind = 0;
        }
        spin_unlock_irqrestore(&msi_lock, flags);
        return -1;
    }
    r->entry = entry;
    mmio_write32(entry + 12, mmio_read32(entry + 12) | 1u);
    mmio_write32(entry, address);
    mmio_write32(entry + 4, 0);
    mmio_write32(entry + 8, data);
    mmio_write32(entry + 12, mmio_read32(entry + 12) & ~1u);
    r->handle = handle;
    if (old >= 0) {
        msi_remap->unmap(old);
    }
    spin_unlock_irqrestore(&msi_lock, flags);
    return 0;
}

/** The entry is masked before its message is written back in the
 * compatibility format and its remapping entry given back. */
void pci_msix_entry_clear(const pci_device_t *dev, unsigned int index)
{
    uint32_t flags = spin_lock_irqsave(&msi_lock);
    struct msi_record *r = msi_record_at(dev, MSI_KIND_MSIX, index);

    if (r != NULL) {
        mmio_write32(r->entry + 12, mmio_read32(r->entry + 12) | 1u);
        msi_record_drop(r);
    }
    spin_unlock_irqrestore(&msi_lock, flags);
}

void pci_msix_unmask(const pci_device_t *dev)
{
    uint8_t cap = pci_msix_capability(dev);
    uint16_t control;

    if (cap == 0) {
        return;
    }
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)(control & ~PCI_MSIX_MASK_ALL));
}

/** The function mask goes on before the entries are rewritten, so that the
 * device sends nothing between its last remapped message and the end. */
static void msix_disable_locked(const pci_device_t *dev)
{
    uint8_t cap = pci_msix_capability(dev);
    struct msi_record *r;
    uint16_t control;
    int i;

    if (cap == 0) {
        return;
    }
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)(control | PCI_MSIX_MASK_ALL));
    for (i = 0; i < MSI_RECORDS; i++) {
        r = &msi_records[i];
        if (r->kind == MSI_KIND_MSIX && r->dev.bus == dev->bus && r->dev.device == dev->device &&
            r->dev.function == dev->function) {
            msi_record_drop(r);
        }
    }
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)((control | PCI_MSIX_MASK_ALL) & ~PCI_MSIX_ENABLE));
}

/** Mask a function's MSI-X messages and release every recorded entry. */
void pci_msix_disable(const pci_device_t *dev)
{
    uint32_t flags = spin_lock_irqsave(&msi_lock);

    msix_disable_locked(dev);
    spin_unlock_irqrestore(&msi_lock, flags);
}

void pci_msi_disable(const pci_device_t *dev)
{
    uint32_t flags = spin_lock_irqsave(&msi_lock);

    msi_disable_locked(dev);
    spin_unlock_irqrestore(&msi_lock, flags);
}

int pci_msi_remap_start(const struct pci_msi_remap *ops)
{
    uint32_t flags = spin_lock_irqsave(&msi_lock);
    int i, j;

    for (i = 0; i < MSI_RECORDS; i++) {
        struct msi_record *r = &msi_records[i];
        uint32_t address, data;

        if (r->kind == 0) {
            continue;
        }
        r->handle = ops->map(&r->dev, r->address, r->data, r->destination, &address, &data);
        if (r->handle < 0) {
            for (j = 0; j < i; j++) {
                if (msi_records[j].kind != 0 && msi_records[j].handle >= 0) {
                    msi_record_write(&msi_records[j], msi_records[j].address, msi_records[j].data,
                                     0);
                    ops->unmap(msi_records[j].handle);
                    msi_records[j].handle = -1;
                }
            }
            spin_unlock_irqrestore(&msi_lock, flags);
            return -1;
        }
        msi_record_write(r, address, data, 1);
    }
    msi_remap = ops;
    spin_unlock_irqrestore(&msi_lock, flags);
    return 0;
}

void pci_msi_remap_stop(void)
{
    uint32_t flags = spin_lock_irqsave(&msi_lock);
    int i;

    for (i = 0; i < MSI_RECORDS; i++) {
        struct msi_record *r = &msi_records[i];

        if (r->kind != 0 && r->handle >= 0 && msi_remap != NULL) {
            msi_record_write(r, r->address, r->data, 0);
            msi_remap->unmap(r->handle);
            r->handle = -1;
        }
    }
    msi_remap = NULL;
    spin_unlock_irqrestore(&msi_lock, flags);
}

/* The registers of the PCI Express capability the reset and the saved
 * state use, by their offsets in it, and their bits. */
#define PCIE_FLAGS          0x02
#define PCIE_DEVCAP         0x04
#define PCIE_DEVCAP_FLR     (1u << 28)
#define PCIE_DEVCTL         0x08
#define PCIE_DEVCTL_BCR_FLR 0x8000u
#define PCIE_DEVSTA         0x0A
#define PCIE_DEVSTA_TRPND   0x0020u
#define PCIE_DEVCTL2        0x28
#define PCIE_LNKCTL2        0x30

/* The power management capability's control and status word, and the
 * Advanced Features capability's registers. */
#define PCI_PM_CTRL          0x04
#define PCI_PM_STATE_MASK    0x0003u
#define PCI_PM_NO_SOFT_RESET 0x0008u
#define PCI_AF_CAP           0x03
#define PCI_AF_CAP_TP        0x01u
#define PCI_AF_CAP_FLR       0x02u
#define PCI_AF_CTRL          0x04
#define PCI_AF_STATUS        0x05

/* Linux's bound on a function's return from a reset,
 * PCIE_RESET_READY_POLL_MS. */
#define PCI_READY_MS 60000u

int pci_reset_method(const pci_device_t *dev)
{
    uint8_t pcie = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PCIE);
    uint8_t af = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_AF);
    uint8_t pm = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PM);
    uint8_t afcap;

    if (pcie != 0 && pcie <= 0xF0u &&
        (pci_read_config32(dev->bus, dev->device, dev->function, (uint8_t)(pcie + PCIE_DEVCAP)) &
         PCIE_DEVCAP_FLR) != 0) {
        return PCI_RESET_FLR;
    }
    if (af != 0 && af <= 0xF8u) {
        afcap = pci_read_config8(dev->bus, dev->device, dev->function, (uint8_t)(af + PCI_AF_CAP));
        if ((afcap & (PCI_AF_CAP_TP | PCI_AF_CAP_FLR)) == (PCI_AF_CAP_TP | PCI_AF_CAP_FLR)) {
            return PCI_RESET_AF_FLR;
        }
    }
    if (pm != 0 && pm <= 0xF8u &&
        (pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(pm + PCI_PM_CTRL)) &
         PCI_PM_NO_SOFT_RESET) == 0) {
        return PCI_RESET_PM;
    }
    return PCI_RESET_NONE;
}

/** The PCI Express control registers the saved state keeps, by their
 * offsets in the capability: device control, link control, device control 2
 * and link control 2, the last two only in a capability of version 2. */
static const uint8_t pcie_saved[4] = {PCIE_DEVCTL, PCIE_LINK_CONTROL, PCIE_DEVCTL2, PCIE_LNKCTL2};

void pci_save_state(const pci_device_t *dev, struct pci_saved_state *st)
{
    uint8_t pcie = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PCIE);
    int i, n;

    for (i = 0; i < 16; i++) {
        st->header[i] = pci_read_config32(dev->bus, dev->device, dev->function, (uint8_t)(i * 4));
    }
    st->pcie_cap = pcie <= 0xC8u ? pcie : 0;
    n = 0;
    if (st->pcie_cap != 0) {
        n = (pci_read_config16(dev->bus, dev->device, dev->function,
                               (uint8_t)(st->pcie_cap + PCIE_FLAGS)) &
             0xFu) >= 2
                ? 4
                : 2;
    }
    st->pcie_count = (uint8_t)n;
    for (i = 0; i < n; i++) {
        st->pcie[i] = pci_read_config16(dev->bus, dev->device, dev->function,
                                        (uint8_t)(st->pcie_cap + pcie_saved[i]));
    }
}

/** The PCI Express registers first, then the header from its end down, as
 * Linux's pci_restore_state, so that the BARs hold their addresses before
 * the command word turns decoding on; the identity dword is read-only, and
 * the command word is written alone, since the status word beside it
 * clears the bits written as one. */
void pci_restore_state(const pci_device_t *dev, const struct pci_saved_state *st)
{
    int i;

    for (i = 0; i < st->pcie_count; i++) {
        pci_write_config16(dev->bus, dev->device, dev->function,
                           (uint8_t)(st->pcie_cap + pcie_saved[i]), st->pcie[i]);
    }
    for (i = 15; i >= 2; i--) {
        if (pci_read_config32(dev->bus, dev->device, dev->function, (uint8_t)(i * 4)) !=
            st->header[i]) {
            pci_write_config32(dev->bus, dev->device, dev->function, (uint8_t)(i * 4),
                               st->header[i]);
        }
    }
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, (uint16_t)st->header[1]);
}

/**
 * Wait for the bit `mask` of the word at `where` to clear, as Linux's
 * pci_wait_for_pending: read at once, then after 100, 200 and 400 ms. 1
 * when it cleared, 0 when it did not.
 */
static int wait_for_pending(const pci_device_t *dev, uint8_t where, uint16_t mask,
                            pci_sleep_t msleep)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (i > 0) {
            msleep(100u << (i - 1));
        }
        if ((pci_read_config16(dev->bus, dev->device, dev->function, where) & mask) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Wait for `dev` to answer after a reset, as Linux's pci_dev_wait: its
 * identity read until it is neither all ones, as from a function not yet
 * back, nor vendor 0x0001, a request retried; the waits double from 1 ms,
 * and stop past PCI_READY_MS. 0, or -ETIMEDOUT.
 */
static int wait_ready(const pci_device_t *dev, pci_sleep_t msleep)
{
    unsigned int delay = 1, waited = 0;
    uint32_t id;

    for (;;) {
        id = pci_read_config32(dev->bus, dev->device, dev->function, PCI_VENDOR_ID);
        if (id != 0xFFFFFFFFu && (id & 0xFFFFu) != 0x0001u) {
            return 0;
        }
        if (waited > PCI_READY_MS) {
            klog(KLOG_WARN, "pci", "reset: %x:%x.%d not ready after %u ms", dev->bus, dev->device,
                 dev->function, waited);
            return -ETIMEDOUT;
        }
        msleep(delay);
        waited += delay;
        delay *= 2;
    }
}

/**
 * The reset itself, by `method`, from Linux's pcie_flr, pci_af_flr and
 * pci_pm_reset: a function-level reset waits for the transactions in
 * flight and 100 ms after it is started; a reset through power management
 * puts the function in D3hot for 10 ms and back in D0 for 10 ms.
 */
static void reset_by(const pci_device_t *dev, int method, pci_sleep_t msleep)
{
    uint8_t cap;
    uint16_t v;

    if (method == PCI_RESET_FLR) {
        cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PCIE);
        if (!wait_for_pending(dev, (uint8_t)(cap + PCIE_DEVSTA), PCIE_DEVSTA_TRPND, msleep)) {
            klog(KLOG_WARN, "pci", "reset: %x:%x.%d: transactions pending, resetting anyway",
                 dev->bus, dev->device, dev->function);
        }
        v = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + PCIE_DEVCTL));
        pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + PCIE_DEVCTL),
                           (uint16_t)(v | PCIE_DEVCTL_BCR_FLR));
        msleep(100);
    } else if (method == PCI_RESET_AF_FLR) {
        cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_AF);
        /* The status byte's TP is bit 8 of the word at the control byte. */
        if (!wait_for_pending(dev, (uint8_t)(cap + PCI_AF_CTRL), 0x0100u, msleep)) {
            klog(KLOG_WARN, "pci", "reset: %x:%x.%d: transactions pending, resetting anyway",
                 dev->bus, dev->device, dev->function);
        }
        pci_write_config8(dev->bus, dev->device, dev->function, (uint8_t)(cap + PCI_AF_CTRL), 1u);
        msleep(100);
    } else {
        cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PM);
        v = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + PCI_PM_CTRL));
        v = (uint16_t)(v & ~PCI_PM_STATE_MASK);
        pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + PCI_PM_CTRL),
                           (uint16_t)(v | 3u));
        msleep(10);
        pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + PCI_PM_CTRL), v);
        msleep(10);
    }
}

/** The power management capability's offset in `dev`, or 0 for none or
 * one too close to the end of the space for its control word. */
static uint8_t pm_cap(const pci_device_t *dev)
{
    uint8_t pm = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_PM);

    return pm <= 0xF8u ? pm : 0;
}

int pci_power_state(const pci_device_t *dev)
{
    uint8_t pm = pm_cap(dev);

    if (pm == 0) {
        return -1;
    }
    return (
        int)(pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(pm + PCI_PM_CTRL)) &
             PCI_PM_STATE_MASK);
}

int pci_set_power_state(const pci_device_t *dev, int state)
{
    uint8_t pm = pm_cap(dev);
    uint16_t v;

    if (pm == 0) {
        return -1;
    }
    v = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(pm + PCI_PM_CTRL));
    /* Bit 15, PME status, clears when written as one. */
    v = (uint16_t)((v & ~(PCI_PM_STATE_MASK | 0x8000u)) | ((uint16_t)state & PCI_PM_STATE_MASK));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(pm + PCI_PM_CTRL), v);
    return 0;
}

/**
 * As Linux's pci_reset_function: the state saved and the function quieted,
 * every decoding, bus mastering and its line off; the reset; the wait for
 * the function to answer; the state restored; and the identity read back,
 * which must be the one saved.
 */
int pci_reset_function(const pci_device_t *dev, pci_sleep_t msleep)
{
    struct pci_saved_state st;
    int method = pci_reset_method(dev);
    int r;

    if (method == PCI_RESET_NONE) {
        return -ENOTTY;
    }
    pci_save_state(dev, &st);
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, PCI_CMD_INTERRUPT_DIS);
    reset_by(dev, method, msleep);
    r = wait_ready(dev, msleep);
    if (r != 0) {
        return r;
    }
    pci_restore_state(dev, &st);
    if (pci_read_config32(dev->bus, dev->device, dev->function, PCI_VENDOR_ID) != st.header[0]) {
        klog(KLOG_WARN, "pci", "reset: %x:%x.%d came back as another device", dev->bus, dev->device,
             dev->function);
        return -EIO;
    }
    klog(KLOG_INFO, "pci", "reset: %x:%x.%d by %s", dev->bus, dev->device, dev->function,
         method == PCI_RESET_FLR      ? "function-level reset"
         : method == PCI_RESET_AF_FLR ? "advanced features function-level reset"
                                      : "power management");
    return 0;
}

/** Find the enabled message on `vector`, with msi_lock held. */
static struct msi_record *msi_vector(int vector)
{
    int i;

    for (i = 0; i < MSI_RECORDS; i++) {
        if (msi_records[i].kind && (int)(msi_records[i].data & 255u) == vector) {
            return &msi_records[i];
        }
    }
    return NULL;
}

/** Read both masks under the same lock that serializes the device. */
int pci_irq_affinity(int vector, uint64_t *requested, uint64_t *effective)
{
    uint32_t flags = spin_lock_irqsave(&msi_lock);
    struct msi_record *r = msi_vector(vector);
    int rc = r ? 0 : -ENOENT;

    if (r) {
        *requested = r->affinity & msi_possible;
        *effective = (uint64_t)1 << r->cpu;
    }
    spin_unlock_irqrestore(&msi_lock, flags);
    return rc;
}

/** Retarget one live message under msi_lock, retaining its vector on all
 * processors. Only the address changes without remapping. An old message
 * therefore invokes the same chain, serialized by irq_dispatch_edge.
 * A remapped move changes the destination in the same remapping entry
 * and invalidates its cached copy; an in-flight message keeps its handle. */
static int msi_retarget(struct msi_record *r, int cpu)
{
    uint32_t address, data, destination, wire_address, wire_data;
    int handle = -1;

    pci_msi_message_cpu((uint8_t)r->data, cpu, &address, &data, &destination);
    wire_address = address;
    wire_data = data;
    if (msi_remap && r->handle >= 0) {
        msi_remap->move(r->handle, destination);
        handle = r->handle;
    } else {
        msi_record_write(r, wire_address, wire_data, 0);
        if (r->kind == MSI_KIND_MSIX) {
            (void)mmio_read32(r->entry);
        } else {
            (void)pci_read_config32(r->dev.bus, r->dev.device, r->dev.function,
                                    (uint8_t)(r->cap + 4));
        }
    }
    r->handle = handle;
    r->address = address;
    r->data = data;
    r->destination = destination;
    r->cpu = cpu;
    return 0;
}

/** Move one enabled vector to an online processor in the requested mask. */
int pci_irq_set_affinity(int vector, uint64_t mask)
{
    uint32_t flags = spin_lock_irqsave(&msi_lock);
    struct msi_record *r = msi_vector(vector);
    int cpu = msi_choose(mask, r);
    int rc = !r ? -ENOENT : cpu < 0 ? -EINVAL : msi_retarget(r, cpu);

    if (!rc) {
        r->affinity = mask;
    }
    spin_unlock_irqrestore(&msi_lock, flags);
    return rc;
}

/** Spread early boot messages, or move a message off an offline target.
 * A request with no processor left online falls back to all online CPUs. */
void pci_msi_rebalance(void)
{
    uint32_t flags = spin_lock_irqsave(&msi_lock);
    int i;

    for (i = 0; i < MSI_RECORDS; i++) {
        if (msi_records[i].kind) {
            msi_records[i].cpu = -1;
        }
    }
    for (i = 0; i < MSI_RECORDS; i++) {
        struct msi_record *r = &msi_records[i];
        int cpu;

        if (!r->kind) {
            continue;
        }
        cpu = msi_choose(r->affinity, r);
        if (cpu < 0) {
            cpu = msi_choose(~(uint64_t)0, r);
        }
        if (cpu >= 0 && cpu != r->cpu) {
            (void)msi_retarget(r, cpu);
        }
    }
    spin_unlock_irqrestore(&msi_lock, flags);
}

/** Move messages before shutdown stops processors, then publish the
 * vector destinations their stop paths use to forward pending requests. */
void pci_msi_shutdown(int cpu)
{
    uint32_t flags = spin_lock_irqsave(&msi_lock);
    int i;

    for (i = 0; i < MSI_RECORDS; i++) {
        struct msi_record *r = &msi_records[i];

        if (r->kind) {
            if (r->cpu != cpu) {
                (void)msi_retarget(r, cpu);
            }
            msi_stop_targets[r->data & 255u] = (unsigned int)cpu + 1u;
        }
    }
    spin_unlock_irqrestore(&msi_lock, flags);
}

/** Read the shutdown destination without taking a lock in a stop NMI. */
int pci_msi_stop_target(int vector)
{
    return vector >= 0 && vector < 256 ? (int)msi_stop_targets[vector] - 1 : -1;
}
