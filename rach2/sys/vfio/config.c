/*
 * config.c: a vfio-pci device's configuration space as its owner sees it,
 * Linux's vfio_pci_config.c in small: the BARs, the ROM, the line, the
 * cache line and latency, the Interrupt Disable bit and the MSI capability
 * are VFIO's own; the MSI-X enable and mask bits too; the bits that reset
 * the function are taken out of writes; a write of the power state is
 * VFIO's to carry out; the rest is the device's.
 */

#include "vfio.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"

#define CMD_IO       0x0001u
#define CMD_MEMORY   0x0002u
#define CMD_INTX_OFF 0x0400u

#define PCI_BAR0     0x10
#define PCI_ROM      0x30
#define PCI_ROM_ON   0x1u
#define PCI_PIN      0x3D
#define PCI_ASLS     0xFC /* the IGD's OpRegion address */
#define PCI_CAP_PTR  0x34
#define PCI_CAP_VNDR 0x09

/** The device's function. */
#define DEV(d) (d)->pci.bus, (d)->pci.device, (d)->pci.function

/** Mark `len` bytes of the map from `at` with `how`. */
static void mark(struct vfio_device *d, unsigned int at, unsigned int len, uint8_t how)
{
    unsigned int i;

    for (i = at; i < at + len && i < 256u; i++) {
        d->cfg_map[i] = how;
    }
}

/** The bytes of the capability `id` at `at`, as Linux's vfio_cap_len
 * finds them; 4 for one it does not know. */
static unsigned int cap_length(const struct vfio_device *d, uint8_t id, uint8_t at)
{
    uint16_t flags = pci_read_config16(DEV(d), (uint8_t)(at + 2));
    unsigned int len;

    switch (id) {
    case PCI_CAP_ID_PM:
        return 8;
    case PCI_CAP_ID_MSI:
        len = 10;
        len += (flags & 0x0080u) ? 4u : 0u;
        len += (flags & 0x0100u) ? 10u : 0u;
        return len;
    case PCI_CAP_ID_MSIX:
        return 12;
    case PCI_CAP_ID_PCIE:
        return (flags & 0xFu) == 1 ? 0x14u : 0x3Cu;
    case PCI_CAP_ID_AF:
        return 6;
    case PCI_CAP_VNDR:
        len = pci_read_config8(DEV(d), (uint8_t)(at + 2));
        return len < 3 ? 3u : len;
    default:
        return 4;
    }
}

/** Mark the bytes of the capability `id` at `at`. */
static void mark_cap(struct vfio_device *d, uint8_t id, uint8_t at)
{
    unsigned int len = cap_length(d, id, at);

    mark(d, at, len, CFG_RO);
    switch (id) {
    case PCI_CAP_ID_PM:
        mark(d, at + 4u, 1, CFG_VIRT);
        mark(d, at + 5u, 1, CFG_RAW);
        d->pm_cap = at;
        break;
    case PCI_CAP_ID_MSI:
        mark(d, at + 2u, len - 2u, CFG_VIRT);
        d->msi_cap = at;
        break;
    case PCI_CAP_ID_MSIX:
        mark(d, at + 3u, 1, CFG_VIRT);
        d->msix_cap = at;
        break;
    case PCI_CAP_ID_PCIE:
        mark(d, at + 4u, len - 4u, CFG_RAW);
        mark(d, at + 9u, 1, CFG_VIRT);
        d->pcie_cap = at;
        break;
    case PCI_CAP_ID_AF:
        mark(d, at + 4u, 1, CFG_VIRT);
        d->af_cap = at;
        break;
    default:
        break;
    }
}

/**
 * Make the map of the header and the standard capabilities, and the
 * virtual registers from what the device holds now, with its messages off
 * and the MSI-X bits clear. Bytes outside the header and every capability
 * are the device's, as Linux leaves them.
 */
void vfio_config_init(struct vfio_device *d)
{
    uint8_t at, id;
    unsigned int i;
    int hops;

    mark(d, 0, 256, CFG_RAW);
    mark(d, 0x00, 0x40, CFG_RO);
    mark(d, 0x04, 2, CFG_VIRT);
    mark(d, 0x0C, 2, CFG_VIRT);
    mark(d, PCI_BAR0, 24, CFG_VIRT);
    mark(d, PCI_ROM, 4, CFG_VIRT);
    mark(d, PCI_INTERRUPT_LINE, 2, CFG_VIRT);
    d->msi_cap = d->msix_cap = d->pcie_cap = d->af_cap = d->pm_cap = 0;
    if (pci_read_config16(DEV(d), PCI_STATUS) & PCI_STATUS_CAP_LIST) {
        at = (uint8_t)(pci_read_config8(DEV(d), PCI_CAP_PTR) & 0xFCu);
        for (hops = 0; hops < 48 && at >= 0x40; hops++) {
            id = pci_read_config8(DEV(d), at);
            mark_cap(d, id, at);
            at = (uint8_t)(pci_read_config8(DEV(d), (uint8_t)(at + 1)) & 0xFCu);
        }
    }
    if (d->igd.present) {
        mark(d, PCI_ASLS, 4, CFG_RO);
    }
    for (i = 0; i < 6; i++) {
        uint32_t v = pci_read_config32(DEV(d), (uint8_t)(PCI_BAR0 + 4 * i));

        d->vbar[i] = v;
        d->bar_flags[i] = (uint8_t)((v & 1u) ? (v & 0x3u) : (v & 0xFu));
    }
    d->vrom = pci_read_config32(DEV(d), PCI_ROM) & ~PCI_ROM_ON;
    d->vcmd_intx = (uint16_t)(pci_read_config16(DEV(d), PCI_COMMAND) & CMD_INTX_OFF);
    d->vcache = pci_read_config8(DEV(d), PCI_CACHE_LINE_SIZE);
    d->vlatency = pci_read_config8(DEV(d), 0x0D);
    d->vline = pci_read_config8(DEV(d), PCI_INTERRUPT_LINE);
    k_memset(d->vmsi, 0, sizeof(d->vmsi));
    for (i = 0; d->msi_cap != 0 && i < sizeof(d->vmsi); i++) {
        d->vmsi[i] = pci_read_config8(DEV(d), (uint8_t)(d->msi_cap + i));
    }
    d->vmsi[2] &= (uint8_t)~0x71u; /* MSI off, one message */
    d->vmsix_ctl = 0;
}

/** The command word to write for the owner's `cmd`: Interrupt Disable on
 * while the owner sets it, while the line is masked, and while the line is
 * not the device's interrupt. */
uint16_t vfio_config_command(const struct vfio_device *d, uint16_t cmd)
{
    cmd = (uint16_t)(cmd & ~CMD_INTX_OFF);
    if (d->vcmd_intx || d->intx_masked || d->irq_type != VFIO_PCI_INTX_IRQ_INDEX) {
        cmd |= CMD_INTX_OFF;
    }
    return cmd;
}

/** BAR `i` as the owner reads it: what it wrote, cut to the BAR's size,
 * with the BAR's kind; the upper half of a 64-bit BAR cut to its own. */
static uint32_t vbar_value(const struct vfio_device *d, int i)
{
    uint64_t size;

    if (i < 0 || i > 5) {
        return 0;
    }
    size = d->bar_size[i];
    if (size == 0 && i > 0 && (d->bar_flags[i - 1] & 0x7u) == 0x4u && d->bar_size[i - 1] != 0) {
        return d->vbar[i] & ~(uint32_t)((d->bar_size[i - 1] - 1) >> 32);
    }
    if (size == 0) {
        return 0;
    }
    if (d->bar_io[i]) {
        return (d->vbar[i] & ~(uint32_t)(size - 1) & ~0x3u) | d->bar_flags[i];
    }
    return (d->vbar[i] & ~(uint32_t)(size - 1) & ~0xFu) | d->bar_flags[i];
}

/** The ROM's register as the owner reads it. */
static uint32_t vrom_value(const struct vfio_device *d)
{
    if (d->rom_size == 0) {
        return 0;
    }
    return (d->vrom & ~(d->rom_size - 1u) & 0xFFFFF800u) | (d->vrom & PCI_ROM_ON);
}

/** The byte `off` of a register VFIO keeps in a dword: `v` shifted. */
static uint8_t byte_of(uint32_t v, unsigned int off)
{
    return (uint8_t)(v >> (8 * (off & 3u)));
}

/** A virtual byte of the header or of a capability, `phys` being the
 * device's own. */
static uint8_t virt_read8(const struct vfio_device *d, unsigned int off, uint8_t phys)
{
    if (off == 0x04) {
        return phys;
    }
    if (off == 0x05) {
        return (uint8_t)((phys & ~(CMD_INTX_OFF >> 8)) | (d->vcmd_intx >> 8));
    }
    if (off == 0x0C) {
        return d->vcache;
    }
    if (off == 0x0D) {
        return d->vlatency;
    }
    if (off >= PCI_BAR0 && off < PCI_BAR0 + 24) {
        return byte_of(vbar_value(d, (int)((off - PCI_BAR0) / 4)), off);
    }
    if (off >= PCI_ROM && off < PCI_ROM + 4) {
        return byte_of(vrom_value(d), off);
    }
    if (off == PCI_INTERRUPT_LINE) {
        return d->vline;
    }
    if (off == PCI_PIN) {
        return d->intx_ok ? phys : 0;
    }
    if (d->msi_cap != 0 && off >= d->msi_cap && off < d->msi_cap + sizeof(d->vmsi)) {
        return d->vmsi[off - d->msi_cap];
    }
    if (d->msix_cap != 0 && off == d->msix_cap + 3u) {
        return (uint8_t)((phys & 0x07u) | ((d->vmsix_ctl >> 8) & 0xC0u));
    }
    return phys;
}

/** Store `b`, the owner's write of byte `off` of a register `v`. */
static void put_byte(uint32_t *v, unsigned int off, uint8_t b)
{
    unsigned int shift = 8 * (off & 3u);

    *v = (*v & ~(0xFFu << shift)) | ((uint32_t)b << shift);
}

/** The owner's write of the command word's byte `off`. Turning memory
 * decoding off takes the BARs' pages from every mapping of them, as
 * Linux's vfio_pci_zap_bars on the same write. */
static void command_write8(struct vfio_device *d, unsigned int off, uint8_t b)
{
    uint16_t cmd = pci_read_config16(DEV(d), PCI_COMMAND);

    if (off == 0x04) {
        if ((cmd & CMD_MEMORY) && !(b & CMD_MEMORY)) {
            vfio_device_zap(d);
        }
        cmd = (uint16_t)((cmd & 0xFF00u) | b);
    } else {
        d->vcmd_intx = (uint16_t)(((uint16_t)b << 8) & CMD_INTX_OFF);
        cmd = (uint16_t)((cmd & 0x00FFu) | ((uint16_t)b << 8));
    }
    pci_write_config16(DEV(d), PCI_COMMAND, vfio_config_command(d, cmd));
}

/** The owner's write of a virtual byte of a capability. */
static void cap_write8(struct vfio_device *d, unsigned int off, uint8_t b)
{
    if (d->msi_cap != 0 && off >= d->msi_cap && off < d->msi_cap + sizeof(d->vmsi)) {
        unsigned int i = off - d->msi_cap;

        if (i == 2) {
            d->vmsi[2] = (uint8_t)((d->vmsi[2] & ~0x71u) | (b & 0x71u));
        } else if (i != 3) {
            d->vmsi[i] = b;
        }
    } else if (d->msix_cap != 0 && off == d->msix_cap + 3u) {
        d->vmsix_ctl = (uint16_t)(((uint16_t)b << 8) & 0xC000u);
    } else if (d->pcie_cap != 0 && off == d->pcie_cap + 9u) {
        /* Initiate Function Level Reset is VFIO's to do, at a reset. */
        pci_write_config8(DEV(d), (uint8_t)off, (uint8_t)(b & 0x7Fu));
    } else if (d->af_cap != 0 && off == d->af_cap + 4u) {
        pci_write_config8(DEV(d), (uint8_t)off, (uint8_t)(b & 0xFEu));
    } else if (d->pm_cap != 0 && off == d->pm_cap + 4u) {
        /* The power state is VFIO's to change, as Linux's
         * vfio_pm_config_write; the byte's other bits are read-only. */
        vfio_device_power(d, b & 0x3);
    }
}

/** The owner's write of the virtual byte `off`. */
static void virt_write8(struct vfio_device *d, unsigned int off, uint8_t b)
{
    if (off == 0x04 || off == 0x05) {
        command_write8(d, off, b);
    } else if (off == 0x0C) {
        d->vcache = b;
    } else if (off == 0x0D) {
        d->vlatency = b;
    } else if (off >= PCI_BAR0 && off < PCI_BAR0 + 24) {
        put_byte(&d->vbar[(off - PCI_BAR0) / 4], off, b);
    } else if (off >= PCI_ROM && off < PCI_ROM + 4) {
        put_byte(&d->vrom, off, b);
    } else if (off == PCI_INTERRUPT_LINE) {
        d->vline = b;
    } else if (off != PCI_PIN) {
        cap_write8(d, off, b);
    }
}

/** Read the `len` bytes from `pos`, an access of 1, 2 or 4 bytes within a
 * dword, as the owner sees them, into `out`. */
static void chunk_read(const struct vfio_device *d, uint32_t pos, uint32_t len, uint8_t *out)
{
    uint32_t dw, i;

    if (pos >= 256) {
        dw = pci_read_config32_ext(DEV(d), (uint16_t)(pos & ~3u));
    } else {
        dw = pci_read_config32(DEV(d), (uint8_t)(pos & ~3u));
    }
    for (i = 0; i < len; i++) {
        uint32_t off = pos + i;
        uint8_t phys = byte_of(dw, off);

        out[i] = (off < 256 && d->cfg_map[off] == CFG_VIRT) ? virt_read8(d, off, phys) : phys;
    }
}

/** Write the access of `len` bytes at `pos` from `in`: through, at its own
 * width, when every byte is the device's; else a byte at a time. The
 * extended space takes no write, as Linux keeps it read-only for the
 * capabilities it does not know. */
static void chunk_write(struct vfio_device *d, uint32_t pos, uint32_t len, const uint8_t *in)
{
    uint32_t i, raw = 0;

    if (pos >= 256) {
        return;
    }
    for (i = 0; i < len; i++) {
        raw += d->cfg_map[pos + i] == CFG_RAW;
    }
    if (raw == len && len == 4) {
        pci_write_config32(DEV(d), (uint8_t)pos,
                           (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 |
                               (uint32_t)in[3] << 24);
        return;
    }
    if (raw == len && len == 2) {
        pci_write_config16(DEV(d), (uint8_t)pos, (uint16_t)(in[0] | in[1] << 8));
        return;
    }
    for (i = 0; i < len; i++) {
        if (d->cfg_map[pos + i] == CFG_RAW) {
            pci_write_config8(DEV(d), (uint8_t)(pos + i), in[i]);
        } else if (d->cfg_map[pos + i] == CFG_VIRT) {
            virt_write8(d, pos + i, in[i]);
        }
    }
}

/**
 * Read or, with `write`, write `count` bytes of the configuration space
 * from `pos`, a kernel buffer, in accesses of 4, 2 and 1 bytes aligned as
 * Linux's vfio_config_do_rw splits them. The bytes moved, or -EINVAL for a
 * range past the space.
 */
int32_t vfio_config_rw(struct vfio_device *d, uint32_t pos, uint8_t *buf, uint32_t count, int write)
{
    uint32_t done = 0, len;

    if (pos >= d->cfg_size) {
        return -EINVAL;
    }
    if (count > d->cfg_size - pos) {
        count = d->cfg_size - pos;
    }
    while (done < count) {
        uint32_t at = pos + done;

        len = (count - done >= 4 && (at & 3u) == 0)   ? 4u
              : (count - done >= 2 && (at & 1u) == 0) ? 2u
                                                      : 1u;
        if (write) {
            chunk_write(d, at, len, buf + done);
        } else {
            chunk_read(d, at, len, buf + done);
        }
        done += len;
    }
    return (int32_t)done;
}
