/*
 * pci.c: the descriptor of a device given to vfio-pci, as Linux's
 * vfio_pci_core.c gives it: its regions at index << 40, read and written
 * through pread and pwrite and its memory BARs mapped as device memory;
 * its information, interrupts and reset through ioctls; and its state put
 * back, by a reset, once the last descriptor and mapping are gone.
 */

#include "vfio.h"

#include "../fs/fd.h"
#include "../fs/file.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"
#include "../mm/vm.h"

#define CMD_IO       0x0001u
#define CMD_MEMORY   0x0002u
#define CMD_MASTER   0x0004u
#define CMD_INTX_OFF 0x0400u

#define PAGE           0x1000u
#define REGION_OFFSET  ((1ull << VFIO_PCI_OFFSET_SHIFT) - 1)
#define REGION_PGOFF   ((1u << VFIO_PCI_PGOFF_SHIFT) - 1)
#define SHADOW_ROM     0xC0000u /* the firmware's copy of the VGA ROM */
#define SHADOW_ROM_LEN 0x20000u
#define BOUNCE         256u /* the bytes a read or write moves at a time */

/** The device's function. */
#define DEV(d) (d)->pci.bus, (d)->pci.device, (d)->pci.function

/** read of a device's descriptor, at its position. */
static int32_t device_read(fd_entry_t *e, char __user *buf, uint32_t count);
/** write of a device's descriptor, at its position. */
static int32_t device_write(fd_entry_t *e, const char __user *buf, uint32_t count);
static int32_t device_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);
static void device_close(fd_entry_t *e);
static int32_t device_mmap(fd_entry_t *e, struct vma *vma);
static int64_t device_llseek(fd_entry_t *e, int64_t offset, int whence);

static const struct file_ops device_ops = {device_read,  device_write, NULL,          device_ioctl,
                                           device_close, device_mmap,  device_llseek, 0};

int vfio_device_file_is_valid(const struct fd_entry *e)
{
    return e && e->type == FD_TYPE_ANON && e->ops == &device_ops && e->priv;
}

/** Take a reference to `d`, which holds its group. */
static void device_get(struct vfio_device *d)
{
    d->refs++;
    vfio_groups[d->group].dev_open++;
}

/** Drop a reference to `d`; the last releases the device, before its
 * group may leave its container, so that the reset runs with the device
 * still in its domain. */
static void device_put(struct vfio_device *d)
{
    if (--d->refs == 0) {
        vfio_device_release(d);
    }
    vfio_group_put_device(&vfio_groups[d->group]);
}

/**
 * Take the pages of every BAR from each mapping of them, in one walk, as
 * Linux's vfio_pci_zap_bars unmaps BAR 0 to the ROM in one
 * unmap_mapping_range (drivers/vfio/pci/vfio_pci_core.c:1641-1649). Only the
 * walk's last pass, which waits for nothing, runs whole under the big lock
 * (`paging_zap_where`); a walk for each BAR would let an earlier BAR fault
 * back in while a later one's walk waited, and stay mapped across the reset
 * or the end of decoding that follows. The caller sets what `bar_page`
 * refuses before it lets the big lock go.
 */
void vfio_device_zap(struct vfio_device *d)
{
    paging_zap_device(d, 0, 6u << VFIO_PCI_PGOFF_SHIFT);
}

/** Read the BARs: their addresses, sizes and kinds. */
static void bars_init(struct vfio_device *d)
{
    int i;

    for (i = 0; i < 6; i++) {
        uint32_t v = pci_read_config32(DEV(d), (uint8_t)(0x10 + 4 * i));

        d->bar_io[i] = (v & 1u) != 0;
        d->bar_size[i] = pci_bar_size64(&d->pci, i);
        d->bar_phys[i] = d->bar_io[i] ? (v & ~3u) : pci_bar_address64(&d->pci, i);
        if (!d->bar_io[i] && (v & 0x6u) == 0x4u && i < 5) {
            i++;
            d->bar_io[i] = 0;
            d->bar_size[i] = 0;
            d->bar_phys[i] = 0;
        }
    }
}

/** Find the ROM: the ROM BAR's when the device has one with an address,
 * else for the boot display the firmware's shadow at 0xC0000, as Linux's
 * pci_map_rom takes it. */
static void rom_init(struct vfio_device *d)
{
    uint32_t v = pci_read_config32(DEV(d), 0x30), size;

    d->rom_size = 0;
    d->rom_shadow = 0;
    pci_write_config32(DEV(d), 0x30, 0xFFFFF800u);
    size = pci_read_config32(DEV(d), 0x30) & 0xFFFFF800u;
    pci_write_config32(DEV(d), 0x30, v);
    if (size != 0 && (v & 0xFFFFF800u) != 0) {
        d->rom_size = ~size + 1u;
        d->rom_phys = v & 0xFFFFF800u;
    } else if (d->pci.class_code == 0x03 && d->pci.subclass == 0x00 &&
               (pci_read_config16(DEV(d), PCI_COMMAND) & CMD_IO)) {
        d->rom_size = SHADOW_ROM_LEN;
        d->rom_phys = SHADOW_ROM;
        d->rom_shadow = 1;
    }
}

/** Whether the device masks its line by Interrupt Disable, PCI 2.3's
 * bit, which Linux's pci_intx_mask_supported tries; the line must be
 * routed and the pin set. */
static int intx_mask_supported(const struct vfio_device *d)
{
    uint16_t cmd = pci_read_config16(DEV(d), PCI_COMMAND), now;

    if (pci_read_config8(DEV(d), 0x3D) == 0 || d->pci.irq == 0 || d->pci.irq >= 0xF0) {
        return 0;
    }
    pci_write_config16(DEV(d), PCI_COMMAND, (uint16_t)(cmd ^ CMD_INTX_OFF));
    now = pci_read_config16(DEV(d), PCI_COMMAND);
    pci_write_config16(DEV(d), PCI_COMMAND, cmd);
    return ((now ^ cmd) & CMD_INTX_OFF) != 0;
}

/**
 * Make `d` ready for its first descriptor, as Linux's vfio_pci_core_enable:
 * reset it when it can be, keep its state for the release, read its BARs,
 * ROM, capabilities and MSI-X table, and for an Intel display its
 * OpRegion; then decoding on and the line off until the owner takes it.
 * 0, or the OpRegion's error.
 */
int vfio_device_enable(struct vfio_device *d)
{
    uint8_t bar;
    uint32_t off, entries;
    int r;

    d->reset_method = pci_reset_method(&d->pci);
    d->reset_works =
        d->reset_method != PCI_RESET_NONE && pci_reset_function(&d->pci, vfio_hw_msleep) == 0;
    pci_save_state(&d->pci, &d->saved);
    bars_init(d);
    rom_init(d);
    d->intx_ok = intx_mask_supported(d);
    d->irq_type = -1;
    d->intx_masked = 0;
    d->msix_entries = 0;
    if (pci_msix_table(&d->pci, &bar, &off, &entries) == 0) {
        d->msix_bar = bar;
        d->msix_offset = off;
        d->msix_entries = entries;
        d->msix_size = entries * 16u;
    }
    k_memset(&d->igd, 0, sizeof(d->igd));
    if (d->pci.vendor_id == 0x8086 && d->pci.class_code == 0x03) {
        r = vfio_igd_init(d);
        if (r != 0 && r != -ENODEV) {
            klog(KLOG_WARN, "vfio", "open: %s: OpRegion: %d", d->name, r);
            return r;
        }
    }
    vfio_config_init(d);
    d->cfg_size =
        (d->pcie_cap != 0 && pci_read_config32_ext(DEV(d), 0x100) != 0xFFFFFFFFu) ? 4096u : 256u;
    pci_write_config16(DEV(d), PCI_COMMAND,
                       vfio_config_command(d, (uint16_t)(pci_read_config16(DEV(d), PCI_COMMAND) |
                                                         CMD_MEMORY | CMD_IO)));
    return 0;
}

/**
 * Release `d` once nothing holds it, as Linux's vfio_pci_core_disable:
 * every interrupt off, bus mastering off, a reset when one worked at the
 * open, and the state kept then written back; then the identity and the
 * header read back to see the device came back as it was.
 */
void vfio_device_release(struct vfio_device *d)
{
    struct pci_saved_state now;
    int r = 0, i, bad = 0;

    vfio_container_audit(vfio_groups[d->group].container, d->name);
    vfio_irq_disable(d);
    pci_write_config16(
        DEV(d), PCI_COMMAND,
        (uint16_t)((pci_read_config16(DEV(d), PCI_COMMAND) & ~CMD_MASTER) | CMD_INTX_OFF));
    if (d->reset_works) {
        r = pci_reset_function(&d->pci, vfio_hw_msleep);
    }
    pci_restore_state(&d->pci, &d->saved);
    pci_save_state(&d->pci, &now);
    for (i = 0; i < 16; i++) {
        bad += i != 1 && now.header[i] != d->saved.header[i];
    }
    if (r != 0 || bad != 0) {
        klog(KLOG_WARN, "vfio", "release: %s: reset %d, %d registers differ", d->name, r, bad);
    }
    vfio_igd_free(d);
}

/** A descriptor on `d` for the caller, `d` made ready if it is the first. */
int32_t vfio_device_open_fd(struct vfio_device *d)
{
    int32_t fd;
    int r;

    if (d->refs == 0) {
        r = vfio_device_enable(d);
        if (r != 0) {
            return r;
        }
    }
    fd = vfio_hw_fd_new(&device_ops, d);
    if (fd < 0) {
        if (d->refs == 0) {
            vfio_device_release(d);
        }
        return fd;
    }
    device_get(d);
    return fd;
}

/** The last close of a device's descriptor. */
static void device_close(fd_entry_t *e)
{
    struct vfio_device *d = (struct vfio_device *)e->priv;

    e->priv = NULL;
    if (d != NULL) {
        device_put(d);
    }
}

/** lseek of a device's descriptor: from the start or the position. */
static int64_t device_llseek(fd_entry_t *e, int64_t offset, int whence)
{
    int64_t at;

    if (whence == 0) {
        at = offset;
    } else if (whence == 1) {
        at = (int64_t)e->ofd->offset + offset;
    } else {
        return -EINVAL;
    }
    return at < 0 ? -EINVAL : at;
}

/* A transfer of a read or write: its kernel buffer, bytes and direction. */
struct xfer {
    uint8_t *buf;
    uint32_t len;
    int write;
};

/** Move the bytes of `x` between BAR `i`'s window `va`, its byte `at`, and
 * the buffer, in aligned accesses of 4, 2 and 1 bytes; the bytes of the
 * MSI-X table read as 0xFF and take no write, as Linux keeps them. */
static void mmio_rw(const struct vfio_device *d, int i, uintptr_t va, uint64_t at,
                    const struct xfer *io)
{
    uint8_t *buf = io->buf;
    uint32_t len = io->len, done = 0, n;
    int write = io->write;

    while (done < len) {
        uint64_t pos = at + done;
        uintptr_t a = va + done;
        volatile uint8_t *p = (volatile uint8_t *)a;
        int table = i == d->msix_bar && d->msix_entries != 0 && pos >= d->msix_offset &&
                    pos < (uint64_t)d->msix_offset + d->msix_size;

        n = (len - done >= 4 && (pos & 3u) == 0)   ? 4u
            : (len - done >= 2 && (pos & 1u) == 0) ? 2u
                                                   : 1u;
        if (table) {
            n = 1;
            if (!write) {
                buf[done] = 0xFF;
            }
        } else if (n == 4) {
            uint32_t v;

            if (write) {
                k_memcpy(&v, buf + done, 4);
                *(volatile uint32_t *)a = v;
            } else {
                v = *(volatile uint32_t *)a;
                k_memcpy(buf + done, &v, 4);
            }
        } else if (n == 2) {
            uint16_t v;

            if (write) {
                k_memcpy(&v, buf + done, 2);
                *(volatile uint16_t *)a = v;
            } else {
                v = *(volatile uint16_t *)a;
                k_memcpy(buf + done, &v, 2);
            }
        } else if (write) {
            *p = buf[done];
        } else {
            buf[done] = *p;
        }
        done += n;
    }
}

/** The port accesses of an I/O BAR, as `mmio_rw` makes them. */
static void port_rw(uint16_t port, uint8_t *buf, uint32_t len, int write)
{
    uint32_t done = 0;

    while (done < len) {
        uint16_t p = (uint16_t)(port + done);

        if (len - done >= 4 && (p & 3u) == 0) {
            uint32_t v;

            if (write) {
                k_memcpy(&v, buf + done, 4);
                pci_io_write32(p, v);
            } else {
                v = pci_io_read32(p);
                k_memcpy(buf + done, &v, 4);
            }
            done += 4;
        } else if (len - done >= 2 && (p & 1u) == 0) {
            uint16_t v;

            if (write) {
                k_memcpy(&v, buf + done, 2);
                pci_io_write16(p, v);
            } else {
                v = pci_io_read16(p);
                k_memcpy(buf + done, &v, 2);
            }
            done += 2;
        } else {
            if (write) {
                pci_io_write8(p, buf[done]);
            } else {
                buf[done] = pci_io_read8(p);
            }
            done += 1;
        }
    }
}

/** Read or write `len` bytes of BAR `i` at `at`, at most BOUNCE: the
 * bytes moved, -EINVAL past the BAR, -EIO with memory decoding off. */
static int32_t bar_rw(struct vfio_device *d, int i, uint64_t at, uint8_t *buf, uint32_t len,
                      int write)
{
    struct pci_window w;
    struct xfer io;
    uint64_t base;
    uintptr_t va;

    if (d->bar_size[i] == 0 || at >= d->bar_size[i]) {
        return -EINVAL;
    }
    if (len > d->bar_size[i] - at) {
        len = (uint32_t)(d->bar_size[i] - at);
    }
    if (d->bar_io[i]) {
        port_rw((uint16_t)(d->bar_phys[i] + at), buf, len, write);
        return (int32_t)len;
    }
    if (!(pci_read_config16(DEV(d), PCI_COMMAND) & CMD_MEMORY)) {
        return -EIO;
    }
    k_memset(&w, 0, sizeof(w));
    w.bar_size = d->bar_size[i];
    base = at & ~(uint64_t)(PAGE - 1);
    va = pci_bar_map(&d->pci, i, base, (size_t)((at - base + len + PAGE - 1) & ~(PAGE - 1)),
                     PTE_NOCACHE, &w);
    if (va == 0) {
        return -ENOMEM;
    }
    io.buf = buf;
    io.len = len;
    io.write = write;
    mmio_rw(d, i, va + (uintptr_t)(at - base), at, &io);
    pci_bar_unmap(&w);
    return (int32_t)len;
}

/** Read `len` bytes of the ROM at `at`, its decoding on meanwhile for a
 * ROM BAR. */
static int32_t rom_read(struct vfio_device *d, uint64_t at, uint8_t *buf, uint32_t len)
{
    uint32_t rom = 0, base, span;
    uint16_t cmd = 0;
    uintptr_t va;

    if (d->rom_size == 0 || at >= d->rom_size) {
        return -EINVAL;
    }
    if (len > d->rom_size - at) {
        len = (uint32_t)(d->rom_size - at);
    }
    if (!d->rom_shadow) {
        rom = pci_read_config32(DEV(d), 0x30);
        cmd = pci_read_config16(DEV(d), PCI_COMMAND);
        pci_write_config32(DEV(d), 0x30, (uint32_t)d->rom_phys | 1u);
        pci_write_config16(DEV(d), PCI_COMMAND, (uint16_t)(cmd | CMD_MEMORY));
    }
    base = (uint32_t)at & ~(PAGE - 1);
    span = ((uint32_t)at - base + len + PAGE - 1) & ~(PAGE - 1);
    va = vfio_hw_map(d->rom_phys + base, span);
    if (va != 0) {
        k_memcpy(buf, (const void *)(va + ((uint32_t)at - base)), len);
        vfio_hw_unmap(va, span);
    }
    if (!d->rom_shadow) {
        pci_write_config16(DEV(d), PCI_COMMAND, cmd);
        pci_write_config32(DEV(d), 0x30, rom);
    }
    return va != 0 ? (int32_t)len : -ENOMEM;
}

/** Move up to BOUNCE bytes of the region at `pos` through `buf`. */
static int32_t region_rw(struct vfio_device *d, uint64_t pos, uint8_t *buf, uint32_t len, int write)
{
    uint32_t index = (uint32_t)(pos >> VFIO_PCI_OFFSET_SHIFT);
    uint64_t at = pos & REGION_OFFSET;

    if (index <= VFIO_PCI_BAR5_REGION_INDEX) {
        return bar_rw(d, (int)index, at, buf, len, write);
    }
    if (index == VFIO_PCI_ROM_REGION_INDEX) {
        return write ? -EINVAL : rom_read(d, at, buf, len);
    }
    if (index == VFIO_PCI_CONFIG_REGION_INDEX) {
        return at >= d->cfg_size ? -EINVAL : vfio_config_rw(d, (uint32_t)at, buf, len, write);
    }
    if (index >= VFIO_IGD_OPREGION_INDEX && !write && at < 0xFFFFFFFFu) {
        return vfio_igd_read(d, index, (uint32_t)at, buf, len);
    }
    return -EINVAL;
}

/** A read or write of a device's descriptor at its position, which it
 * moves: the bytes moved, or the first error when none moved. */
static int32_t device_rw(fd_entry_t *e, uint8_t __user *ubuf, uint32_t count, int write)
{
    struct vfio_device *d = (struct vfio_device *)e->priv;
    uint8_t buf[BOUNCE];
    uint32_t done = 0;
    int32_t n;

    while (done < count) {
        uint32_t len = count - done > BOUNCE ? BOUNCE : count - done;
        uint64_t pos = e->ofd->offset;

        if (write && copy_from_user(buf, ubuf + done, len) != 0) {
            n = -EFAULT;
        } else {
            n = region_rw(d, pos, buf, len, write);
        }
        if (n > 0 && !write && copy_to_user(ubuf + done, buf, (uint32_t)n) != 0) {
            n = -EFAULT;
        }
        if (n <= 0) {
            return done != 0 ? (int32_t)done : n;
        }
        e->ofd->offset = pos + (uint32_t)n;
        done += (uint32_t)n;
        if ((uint32_t)n < len) {
            break;
        }
    }
    return (int32_t)done;
}

/** read of a device's descriptor, at its position. */
static int32_t device_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return device_rw(e, (uint8_t __user *)buf, count, 0);
}

/** write of a device's descriptor, at its position. */
static int32_t device_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return device_rw(e, (uint8_t __force __user *)(__force uintptr_t)buf, count, 1);
}

/** The physical page `pgoff` of a mapping of a BAR, or -1 while the BAR
 * cannot be reached: memory decoding off, a reset under way, or past the
 * BAR's end. */
static int bar_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    struct vfio_device *d = (struct vfio_device *)obj;
    uint32_t i = pgoff >> VFIO_PCI_PGOFF_SHIFT;
    uint64_t at = (uint64_t)(pgoff & REGION_PGOFF) * PAGE;

    if (i > 5 || d->bar_size[i] == 0 || d->bar_io[i] || at >= d->bar_size[i] || d->resetting ||
        !(pci_read_config16(DEV(d), PCI_COMMAND) & CMD_MEMORY)) {
        return -1;
    }
    *phys = (uintptr_t)(d->bar_phys[i] + at);
    return 0;
}

/** The last mapping of a BAR went: the reference it held. */
static void bar_put(void *obj)
{
    device_put((struct vfio_device *)obj);
}

static const struct vm_device_ops bar_vm_ops = {bar_page, bar_put, NULL, 0};

/** mmap of a memory BAR of a page or more, uncached; the mapping holds the
 * device. -EINVAL for another region or a range past the BAR. */
static int32_t device_mmap(fd_entry_t *e, struct vma *vma)
{
    struct vfio_device *d = (struct vfio_device *)e->priv;
    uint32_t i = vma->pgoff >> VFIO_PCI_PGOFF_SHIFT;
    uint64_t at = (uint64_t)(vma->pgoff & REGION_PGOFF) * PAGE;
    uint64_t len = vma->end - vma->start;
    int32_t r;

    if (i > 5 || d->bar_io[i] || d->bar_size[i] < PAGE ||
        at + len > ((d->bar_size[i] + PAGE - 1) & ~(uint64_t)(PAGE - 1))) {
        return -EINVAL;
    }
    device_get(d);
    r = vm_device_map(vma, &bar_vm_ops, d, VM_PROT_UC);
    if (r != 0) {
        device_put(d);
    }
    return r;
}

/** Add the capability `cap` of `len` bytes after the reply `ri`, as
 * Linux's vfio_info_add_capability: with too little room in its `argsz`,
 * `argsz` says the room needed. 0 or -EFAULT. */
static int32_t cap_put(uintptr_t arg, struct vfio_region_info *ri, const void *cap, uint32_t len)
{
    ri->flags |= VFIO_REGION_INFO_FLAG_CAPS;
    if (ri->argsz < sizeof(*ri) + len) {
        ri->argsz = (uint32_t)(sizeof(*ri) + len);
        return 0;
    }
    if (copy_to_user((void __user *)(arg + sizeof(*ri)), cap, len) != 0) {
        return -EFAULT;
    }
    ri->cap_offset = (uint32_t)sizeof(*ri);
    return 0;
}

/** The size and flags of the standard region `index`. -EINVAL for VGA,
 * which this kernel does not give, as Linux without VGA. */
static int32_t region_of(const struct vfio_device *d, uint32_t index, struct vfio_region_info *ri)
{
    ri->flags = 0;
    if (index <= VFIO_PCI_BAR5_REGION_INDEX) {
        ri->size = d->bar_size[index];
        if (ri->size != 0) {
            ri->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
            if (!d->bar_io[index] && ri->size >= PAGE) {
                ri->flags |= VFIO_REGION_INFO_FLAG_MMAP;
            }
        }
    } else if (index == VFIO_PCI_ROM_REGION_INDEX) {
        ri->size = d->rom_size;
        ri->flags = d->rom_size != 0 ? VFIO_REGION_INFO_FLAG_READ : 0;
    } else if (index == VFIO_PCI_CONFIG_REGION_INDEX) {
        ri->size = d->cfg_size;
        ri->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
    } else if (index >= VFIO_IGD_OPREGION_INDEX &&
               index < VFIO_IGD_OPREGION_INDEX + vfio_igd_regions(d)) {
        ri->size = vfio_igd_region_size(d, index);
        ri->flags = VFIO_REGION_INFO_FLAG_READ;
    } else {
        return -EINVAL;
    }
    return 0;
}

/** VFIO_DEVICE_GET_REGION_INFO, with the IGD's regions' type and the
 * MSI-X BAR's capability that it may be mapped whole. */
static int32_t get_region_info(struct vfio_device *d, uintptr_t arg)
{
    struct vfio_region_info ri;
    struct vfio_region_info_cap_type type;
    struct vfio_info_cap_header msix;
    int32_t err;

    if (copy_from_user(&ri, (const void __user *)arg, sizeof(ri)) != 0) {
        return -EFAULT;
    }
    if (ri.argsz < sizeof(ri)) {
        return -EINVAL;
    }
    err = region_of(d, ri.index, &ri);
    if (err != 0) {
        return err;
    }
    ri.offset = (uint64_t)ri.index << VFIO_PCI_OFFSET_SHIFT;
    ri.cap_offset = 0;
    if (ri.index >= VFIO_IGD_OPREGION_INDEX) {
        k_memset(&type, 0, sizeof(type));
        type.header.id = VFIO_REGION_INFO_CAP_TYPE;
        type.header.version = 1;
        type.type = VFIO_REGION_TYPE_PCI_VENDOR_TYPE | 0x8086u;
        type.subtype = VFIO_REGION_SUBTYPE_INTEL_IGD_OPREGION + ri.index - VFIO_IGD_OPREGION_INDEX;
        err = cap_put(arg, &ri, &type, sizeof(type));
    } else if (d->msix_entries != 0 && ri.index == (uint32_t)d->msix_bar) {
        k_memset(&msix, 0, sizeof(msix));
        msix.id = VFIO_REGION_INFO_CAP_MSIX_MAPPABLE;
        msix.version = 1;
        err = cap_put(arg, &ri, &msix, sizeof(msix));
    }
    if (err != 0) {
        return err;
    }
    return copy_to_user((void __user *)arg, &ri, sizeof(ri)) != 0 ? -EFAULT : 0;
}

/** VFIO_DEVICE_GET_INFO. */
static int32_t get_info(struct vfio_device *d, uintptr_t arg)
{
    struct vfio_device_info di;
    uint32_t minsz = offsetof(struct vfio_device_info, cap_offset);

    if (copy_from_user(&di, (const void __user *)arg, minsz) != 0) {
        return -EFAULT;
    }
    if (di.argsz < minsz) {
        return -EINVAL;
    }
    di.flags = VFIO_DEVICE_FLAGS_PCI | (d->reset_works ? VFIO_DEVICE_FLAGS_RESET : 0u);
    di.num_regions = VFIO_PCI_NUM_REGIONS + vfio_igd_regions(d);
    di.num_irqs = VFIO_PCI_NUM_IRQS;
    return copy_to_user((void __user *)arg, &di, minsz) != 0 ? -EFAULT : 0;
}

/** VFIO_DEVICE_GET_IRQ_INFO. */
static int32_t get_irq_info(struct vfio_device *d, uintptr_t arg)
{
    struct vfio_irq_info ii;

    if (copy_from_user(&ii, (const void __user *)arg, sizeof(ii)) != 0) {
        return -EFAULT;
    }
    if (ii.argsz < sizeof(ii) || ii.index >= VFIO_PCI_NUM_IRQS) {
        return -EINVAL;
    }
    ii.flags = VFIO_IRQ_INFO_EVENTFD;
    ii.count = vfio_irq_count(d, ii.index);
    if (ii.index == VFIO_PCI_INTX_IRQ_INDEX) {
        ii.flags |= VFIO_IRQ_INFO_MASKABLE | VFIO_IRQ_INFO_AUTOMASKED;
    } else if (ii.index != VFIO_PCI_MSIX_IRQ_INDEX) {
        ii.flags |= VFIO_IRQ_INFO_NORESIZE;
    }
    return copy_to_user((void __user *)arg, &ii, sizeof(ii)) != 0 ? -EFAULT : 0;
}

/** VFIO_DEVICE_RESET: the mappings' pages taken, the function reset with
 * its state put back, and its interrupts programmed again. */
static int32_t device_reset(struct vfio_device *d)
{
    int r;

    if (!d->reset_works) {
        return -EINVAL;
    }
    vfio_device_zap(d);
    d->resetting = 1;
    r = pci_reset_function(&d->pci, vfio_hw_msleep);
    d->resetting = 0;
    vfio_irq_restore(d);
    return r;
}

/** The ioctls of a device. */
static int32_t device_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct vfio_device *d = (struct vfio_device *)e->priv;

    switch (request) {
    case VFIO_DEVICE_GET_INFO:
        return get_info(d, arg);
    case VFIO_DEVICE_GET_REGION_INFO:
        return get_region_info(d, arg);
    case VFIO_DEVICE_GET_IRQ_INFO:
        return get_irq_info(d, arg);
    case VFIO_DEVICE_SET_IRQS:
        return vfio_irq_set(d, arg);
    case VFIO_DEVICE_RESET:
        return device_reset(d);
    case VFIO_DEVICE_GET_PCI_HOT_RESET_INFO:
    case VFIO_DEVICE_PCI_HOT_RESET:
        return -ENODEV; /* no slot or bus reset, as on a root bus */
    default:
        return -ENOTTY;
    }
}
