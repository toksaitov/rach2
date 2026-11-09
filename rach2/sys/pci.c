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
#include "lib/kirq.h"
#include "lib/kstring.h"
#include "mm/kheap.h"

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
 * The list holds each function's place, identity, class and header type;
 * what may change after the walk (the BARs, the interrupt line, a bridge's
 * bus numbers) is read again when a search returns it. The first search is
 * the page cache's window probe, early in the boot and before a second
 * processor runs, so the walk needs no lock. No function is added or
 * removed after it: the kernel has no PCI hot plug.
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
 * Append a function to the list, reading its class; false, with the reason
 * in the log, when the list is full and no longer one can be had.
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

void pci_enable_bus_master(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
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
    cmd |= PCI_CMD_MEMORY_SPACE;
    pci_write_config16(dev->bus, dev->device, dev->function, PCI_COMMAND, cmd);
}

/** The Interrupt Disable bit masks the line while it is set. */
void pci_enable_interrupts(pci_device_t *dev)
{
    uint16_t cmd = pci_read_config16(dev->bus, dev->device, dev->function, PCI_COMMAND);
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
 * one.
 *
 * `last` is the last byte the writes reach: the data's, or with per-vector
 * masking the mask register's, past the data. A capability that would run
 * past the 256 bytes an offset can name fails the call, since the writes
 * would wrap around onto the identity registers at offset zero.
 */
int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    uint8_t cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSI);
    uint8_t msix = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSIX);
    uint16_t control;
    uint32_t address, data;
    unsigned int last;

    if (cap == 0) {
        klog(KLOG_WARN, "pci", "msi: %x:%x.%d has no msi capability", dev->bus, dev->device,
             dev->function);
        return -1;
    }
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

    control = (uint16_t)(control & ~(PCI_MSI_ENABLE | PCI_MSI_MULTIPLE_ENABLE));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2), control);
    if (msix != 0) {
        uint16_t mxc = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(msix + 2));

        if ((mxc & PCI_MSIX_ENABLE) != 0) {
            /* MSI beside an enabled MSI-X is undefined by the specification. */
            pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(msix + 2),
                               (uint16_t)(mxc & ~PCI_MSIX_ENABLE));
        }
    }
    pci_msi_message(vector, &address, &data);
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
    klog(KLOG_DEBUG, "pci", "msi: %x:%x.%d on vector %d", dev->bus, dev->device, dev->function,
         vector);
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

/** MSI goes down first, since a device with both enabled is undefined. */
int pci_msix_hold(const pci_device_t *dev)
{
    uint8_t cap = pci_msix_capability(dev);
    uint16_t control;

    if (cap == 0) {
        return -1;
    }
    pci_msi_disable(dev);
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)(control | PCI_MSIX_ENABLE | PCI_MSIX_MASK_ALL));
    return 0;
}

/**
 * MSI-X is held first, enabled with every vector masked, because some
 * devices answer their table only while MSI-X is enabled; the entry is
 * written with the same address and data MSI sends, and its own mask
 * cleared; and the function mask comes off last, when the entry is whole.
 */
int pci_msix_enable(const pci_device_t *dev, uintptr_t table, uint8_t vector)
{
    uint8_t cap = pci_msix_capability(dev);
    uint16_t control;
    uint32_t address, data;

    if (!table || pci_msix_hold(dev) != 0) {
        klog(KLOG_WARN, "pci", "msi-x: %x:%x.%d has no usable msi-x capability", dev->bus,
             dev->device, dev->function);
        return -1;
    }
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    pci_msi_message(vector, &address, &data);
    mmio_write32(table, address);
    mmio_write32(table + 4, 0);
    mmio_write32(table + 8, data);
    mmio_write32(table + 12, 0);
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)((control | PCI_MSIX_ENABLE) & ~PCI_MSIX_MASK_ALL));
    klog(KLOG_DEBUG, "pci", "msi-x: %x:%x.%d on vector %d", dev->bus, dev->device, dev->function,
         vector);
    return 0;
}

void pci_msi_disable(const pci_device_t *dev)
{
    uint8_t cap = pci_find_capability(dev->bus, dev->device, dev->function, PCI_CAP_ID_MSI);
    uint16_t control;

    if (cap == 0) {
        return;
    }
    control = pci_read_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2));
    pci_write_config16(dev->bus, dev->device, dev->function, (uint8_t)(cap + 2),
                       (uint16_t)(control & ~PCI_MSI_ENABLE));
}
