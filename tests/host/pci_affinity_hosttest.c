/*
 * pci_affinity_hosttest.c: allocate and move actual PCI MSI messages over
 * the configuration-port model, with four logical processors.
 *
 * host-deps: rach2/sys/pci.c rach2/sys/arch/x86/pciconf.c
 * host-deps: rach2/sys/arch/x86/vector.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * Only these rows run. The included model's older rows remain uncalled.
 */

int pcimsi_legacy_main(void);
#define main pcimsi_legacy_main
#include "pcimsi_hosttest.c"
#undef main

#include "../../rach2/sys/lib/kerrno.h"

static unsigned int move_count, unmap_count, map_count;
static uint32_t moved_destination;

/** Allocate a model remapping entry and retain the vector as its handle. */
static int remap_message(const pci_device_t *dev, uint32_t address, uint32_t data,
                         uint32_t destination, uint32_t *raddress, uint32_t *rdata)
{
    (void)dev;
    (void)address;
    (void)destination;
    map_count++;
    *raddress = 0xFEE00010u | (data << 5);
    *rdata = 0;
    return (int)data;
}

/** Count a release, which a migration must never request. */
static void unmap_message(int handle)
{
    (void)handle;
    unmap_count++;
}

/** Keep a remapped interrupt's handle live while changing its destination. */
static void move_message(int handle, uint32_t destination)
{
    check(handle >= 48, "a remapped move retains its original vector's handle");
    move_count++;
    moved_destination = destination;
}

/** Include the large x2APIC id which a compatibility message cannot reach. */
static uint64_t remap_cpus(void)
{
    return 0x10000000full;
}

static const struct pci_msi_remap remap_ops = {remap_message, unmap_message, move_message,
                                               remap_cpus};

/** Switching a multi-entry function to MSI drops every MSI-X record. */
static void msix_to_msi_row(void)
{
    pci_device_t dev;
    uint32_t table[8];
    uint64_t requested, effective;

    k_memset(&dev, 0, sizeof(dev));
    k_memset(table, 0, sizeof(table));
    model_reset();
    model_cap(0x90, PCI_CAP_ID_MSIX, 0x34);
    space[0x92] = 1;
    check(pci_msix_hold(&dev) == 0 && pci_msix_entry_set(&dev, (uintptr_t)table, 0, 60) == 0 &&
              pci_msix_entry_set(&dev, (uintptr_t)table, 1, 61) == 0,
          "enable two MSI-X entries on one function");
    pci_msix_unmask(&dev);
    model_cap(0x60, PCI_CAP_ID_MSI, 0x34);
    space[0x62] = PCI_MSI_64BIT;
    model_cap(0x90, PCI_CAP_ID_MSIX, 0x60);
    check(pci_msi_enable(&dev, 48) == 0 && !(cfg16(0x92) & PCI_MSIX_ENABLE),
          "switch the function from MSI-X to MSI");
    check(pci_irq_affinity(60, &requested, &effective) == -ENOENT &&
              pci_irq_affinity(61, &requested, &effective) == -ENOENT,
          "both old vectors disappear from the affinity records");
    pci_msi_disable(&dev);
}

/** Allocate, migrate, rebalance and shut down MSI and MSI-X messages. */
int main(void)
{
    pci_device_t dev[8];
    uint32_t tables[4][4];
    uint64_t requested, effective;
    unsigned int c, i;

    the_apic_id = 2;
    for (c = 0; c < 4; c++) {
        cpu_table[c].online = 1;
        model_apic[c] = 2 + c * 2;
    }
    for (i = 0; i < 8; i++) {
        k_memset(&dev[i], 0, sizeof(dev[i]));
        dev[i].device = (uint8_t)i;
        model_reset();
        if (i < 4) {
            model_cap(0x60, PCI_CAP_ID_MSI, 0x34);
            space[0x62] = 0x80;
            check(pci_msi_enable(&dev[i], (uint8_t)(48 + i)) == 0, "enable an MSI");
            check(cfg16(0x6C) == 48 + i, "allocation preserves the vector");
            check(cfg32(0x64) == (0xFEE00000u | (model_apic[i] << 12)),
                  "the least-allocated processor receives the MSI");
        } else {
            model_cap(0x90, PCI_CAP_ID_MSIX, 0x34);
            k_memset(tables[i - 4], 0, sizeof(tables[0]));
            check(pci_msix_enable(&dev[i], (uintptr_t)tables[i - 4], (uint8_t)(48 + i)) == 0,
                  "enable an MSI-X");
            check(tables[i - 4][0] == (0xFEE00000u | (model_apic[i - 4] << 12)),
                  "MSI-X shares the allocation counts with MSI");
        }
        check(!pci_irq_affinity(48 + (int)i, &requested, &effective) &&
                  effective == ((uint64_t)1 << (i % 4)) && requested == 15,
              "requested and effective affinity name the allocation");
    }
    check(pci_irq_set_affinity(52, 8) == 0 && tables[0][0] == (0xFEE00000u | (8u << 12)) &&
              tables[0][2] == 52,
          "MSI-X migration changes only its address and preserves its vector");
    check(pci_irq_set_affinity(52, 0x10) == -EINVAL,
          "a mask without an online destination leaves the message unchanged");
    check(!pci_irq_affinity(52, &requested, &effective) && requested == 8 && effective == 8,
          "a rejected write preserves both masks");
    check(pci_irq_set_affinity(200, 1) == -ENOENT, "an absent message cannot be moved");
    cpu_table[3].online = 0;
    pci_msi_rebalance();
    check(!pci_irq_affinity(51, &requested, &effective) && !(effective & 8),
          "an unrestricted message leaves the offline processor");
    cpu_table[3].online = 1;
    check(pci_msi_remap_start(&remap_ops) == 0 && map_count == 8, "remap every live message");
    model_apic[32] = 0x1234;
    cpu_table[32].online = 1;
    check(pci_irq_set_affinity(52, (uint64_t)1 << 32) == 0 && move_count == 1 &&
              moved_destination == 0x1234 && !unmap_count && map_count == 8,
          "a remapped move retains the live entry and carries the whole x2APIC id");
    pci_msi_shutdown(0);
    for (i = 0; i < 8; i++) {
        check(!pci_irq_affinity(48 + (int)i, &requested, &effective) && effective == 1,
              "shutdown routes every message to the processor which stays online");
    }
    pci_msi_remap_stop();
    for (i = 0; i < 8; i++) {
        model_reset();
        model_cap(i < 4 ? 0x60 : 0x90, i < 4 ? PCI_CAP_ID_MSI : PCI_CAP_ID_MSIX, 0x34);
        if (i < 4) {
            pci_msi_disable(&dev[i]);
        } else {
            pci_msix_disable(&dev[i]);
        }
        check(pci_irq_affinity(48 + (int)i, &requested, &effective) == -ENOENT,
              "disable removes the affinity record");
    }
    msix_to_msi_row();
    printf("pci_affinity_hosttest: %s\n", fails ? "failed" : "ok");
    return fails != 0;
}
