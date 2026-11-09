/*
 * hw.c: what the VT-d driver needs of the machine, as vtd.h declares it:
 * the units' registers, pages for their tables, the cache, the bus, the
 * firmware's tables and the fault event's vector. A host test gives its
 * own versions, over a model of the units.
 */

#include "vtd.h"

#include "../../../acpi/acpica.h"
#include "../../../cmdline.h"
#include "../../../irq.h"
#include "../../../lib/kstring.h"
#include "../../../mm/paging.h"
#include "../../../pci.h"
#include "../../../udelay.h"
#include "../../arch.h"
#include "../acpi.h"
#include "../lapic.h"

uintptr_t vtd_hw_map(uint64_t base)
{
    return map_mmio((uintptr_t)base, 0x1000);
}

uint32_t vtd_hw_read32(uintptr_t regs, unsigned int off)
{
    return *(volatile uint32_t *)(regs + off);
}

uint64_t vtd_hw_read64(uintptr_t regs, unsigned int off)
{
    return *(volatile uint64_t *)(regs + off);
}

void vtd_hw_write32(uintptr_t regs, unsigned int off, uint32_t v)
{
    *(volatile uint32_t *)(regs + off) = v;
}

void vtd_hw_write64(uintptr_t regs, unsigned int off, uint64_t v)
{
    *(volatile uint64_t *)(regs + off) = v;
}

/** A frame of the kernel's window, whose physical address fits 32 bits. */
void *vtd_hw_page(uint64_t *phys)
{
    void *p = alloc_kernel_frame();

    if (p == NULL) {
        return NULL;
    }
    k_memset(p, 0, 4096);
    *phys = (uint64_t)virt_to_phys(p);
    return p;
}

void vtd_hw_page_free(void *va)
{
    free_kernel_frame(va);
}

void *vtd_hw_virt(uint64_t phys)
{
    return phys_to_virt((uintptr_t)phys);
}

void vtd_hw_flush(const void *p, unsigned long len)
{
    arch_dma_flush(p, (size_t)len);
}

void vtd_hw_udelay(unsigned int us)
{
    k_udelay(us);
}

uint32_t vtd_hw_class(uint8_t bus, uint8_t devfn)
{
    uint8_t dev = (uint8_t)(devfn >> 3), fn = (uint8_t)(devfn & 7u);

    if (pci_read_config16(bus, dev, fn, PCI_VENDOR_ID) == 0xFFFF) {
        return 0xFFFFFFFFu;
    }
    return pci_read_config32(bus, dev, fn, 0x08) >> 8;
}

int vtd_hw_bridge(uint8_t bus, uint8_t devfn, uint8_t *sec, uint8_t *sub)
{
    uint8_t dev = (uint8_t)(devfn >> 3), fn = (uint8_t)(devfn & 7u);

    if (pci_read_config16(bus, dev, fn, PCI_VENDOR_ID) == 0xFFFF ||
        (pci_read_config8(bus, dev, fn, PCI_HEADER_TYPE) & 0x7Fu) != 0x01u) {
        return -1;
    }
    *sec = pci_read_config8(bus, dev, fn, 0x19);
    *sub = pci_read_config8(bus, dev, fn, 0x1A);
    return 0;
}

int vtd_hw_bus_present(uint8_t bus)
{
    uint8_t dev;

    for (dev = 0; dev < 32; dev++) {
        if (pci_read_config16(bus, dev, 0, PCI_VENDOR_ID) != 0xFFFF) {
            return 1;
        }
    }
    return 0;
}

uint64_t vtd_hw_ram_top(void)
{
    return (uint64_t)_frames_number * PAGE_SIZE;
}

const uint8_t *vtd_hw_dmar(uint32_t *len)
{
    return acpica_table("DMAR", len);
}

int vtd_hw_option(const char *key, const char *value)
{
    char buf[64];
    size_t n = k_strlen(value);
    const char *p = buf;

    if (!kcmdline_value(key, buf, (unsigned int)sizeof(buf))) {
        return 0;
    }
    while (*p != 0) {
        const char *end = p;

        while (*end != 0 && *end != ',') {
            end++;
        }
        if ((size_t)(end - p) == n) {
            size_t i = 0;

            while (i < n && p[i] == value[i]) {
                i++;
            }
            if (i == n) {
                return 1;
            }
        }
        p = *end == ',' ? end + 1 : end;
    }
    return 0;
}

int vtd_hw_ioapics(uint8_t *ids, int max)
{
    const struct acpi_info *a = acpi_get();
    int i, n = 0;

    for (i = 0; i < a->nioapics && n < max; i++) {
        ids[n++] = a->ioapics[i].id;
    }
    return n;
}

/** The fault event's handler, on the vector `vtd_hw_fault_vector` took. */
static void fault_isr(isr_params_t *p)
{
    (void)p;
    vtd_fault_all();
}

int vtd_hw_fault_vector(void)
{
    int v;

    if (!isr_msi_ready() || (v = isr_alloc_vector()) < 0) {
        return -1;
    }
    register_isr_handler((uint8_t)v, fault_isr, "dmar");
    return v;
}

uint32_t vtd_hw_apic_id(void)
{
    return lapic_id();
}
