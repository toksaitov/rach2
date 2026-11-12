/*
 * iommu.h: the IOMMU as the rest of the kernel sees it: domains of DMA
 * addresses, devices given to them, the regions a device must keep, and
 * the counts of what the units blocked. On a PC it is Intel's VT-d
 * (sys/arch/x86/iommu/).
 */

#ifndef _IOMMU_H_
#define _IOMMU_H_

#include "lib/kint.h"

/* One address space for DMA; its devices see only what is mapped in it. */
struct iommu_domain;

/* What a mapping lets a device do. */
#define IOMMU_READ  1
#define IOMMU_WRITE 2

/* The kinds of a reserved region, as Linux's reserved_regions names them. */
#define IOMMU_RESV_DIRECT           0 /* the firmware's, mapped one to one */
#define IOMMU_RESV_DIRECT_RELAXABLE 1 /* the same, unused once a VM owns it */
#define IOMMU_RESV_MSI              2 /* where messages go, never DMA */

/* A region of DMA addresses a domain must not map; `end` is its last byte. */
struct iommu_region {
    uint64_t start;
    uint64_t end;
    int type;
};

/** Whether a unit is up, with DMA translation allowed: 0 under
 * `intel_iommu=off` or with no DMAR table. */
int iommu_present(void);

/** Whether the host's interrupts go through the units' remapping tables. */
int iommu_irq_remapped(void);

/** Whether they do, with the entries naming processors by their 32-bit
 * x2APIC ids, so that the local APIC may stay in x2APIC mode on bare
 * metal (`lapic_x2apic_settle`). */
int iommu_irq_remapped_x2apic(void);

/** Hold the units' fault events, which are messages of their own (`hold`
 * 1), and send what came meanwhile (`hold` 0): across the late x2APIC
 * leave, which drops a message that comes while the local APIC is moved
 * (`lapic_x2apic_settle`). */
void iommu_fault_hold(int hold);

/** The unit whose scope has the device at (bus, devfn), its index, or -1;
 * the devices of one unit are translated or not together. */
int iommu_unit_of(uint8_t bus, uint8_t devfn);

/**
 * A new domain with nothing mapped: NULL when no unit is up or memory is
 * short. The caller owns it until `iommu_domain_free`, which it may call
 * only once every device is detached.
 */
struct iommu_domain *iommu_domain_alloc(void);
void iommu_domain_free(struct iommu_domain *d);

/** The end of `d`'s DMA addresses: an IOVA must be below it. */
uint64_t iommu_domain_limit(const struct iommu_domain *d);

/**
 * Map [iova, iova + size) of `d` to physical memory from `phys`, with
 * `prot` of IOMMU_READ and IOMMU_WRITE: 0, -EINVAL for an address or size
 * not of whole 4 KiB pages or past the limit, -EEXIST where a page is
 * already mapped (nothing is then mapped), -ENOMEM. The units see it before
 * the call returns. It may be called with interrupts on, from a process.
 */
int iommu_map(struct iommu_domain *d, uint64_t iova, uint64_t phys, uint64_t size, int prot);

/** Unmap [iova, iova + size) of `d`, whole pages; a page not mapped is
 * passed over. The units no longer reach it when the call returns. */
int iommu_unmap(struct iommu_domain *d, uint64_t iova, uint64_t size);

/** Where `iova` of `d` goes: the physical address, or ~0 when unmapped. */
uint64_t iommu_iova_to_phys(const struct iommu_domain *d, uint64_t iova);

/**
 * Give the device at (bus, devfn) to `d`. Its unit's translation turns on
 * with the first device given, after every other device of the unit has
 * been given a pass-through entry, or the host's identity domain where the
 * unit has no pass-through, so that the host's own DMA goes on. 0; -ENODEV
 * when no unit has the device or translation is off by the boot line;
 * -EPERM when the firmware's reserved region of the device must stay mapped
 * (one not relaxable, as Linux decides); -EBUSY when it is already in a
 * domain; -EINVAL when `d` cannot be walked by the unit; -ENOMEM.
 */
int iommu_attach(struct iommu_domain *d, uint8_t bus, uint8_t devfn);

/** Take the device back from `d` to the host's pass-through; the unit's
 * translation turns off with its last device given back. */
void iommu_detach(struct iommu_domain *d, uint8_t bus, uint8_t devfn);

/**
 * The regions the device at (bus, devfn) must not have mapped, up to `max`
 * in `out`: the firmware's reserved regions whose scope names it, relaxable
 * for a USB or a display device as Linux decides, and the message window
 * 0xFEE00000 to 0xFEEFFFFF. Their count.
 */
int iommu_reserved_regions(uint8_t bus, uint8_t devfn, struct iommu_region *out, int max);

/** The faults the units have reported since the boot: DMA blocked by a
 * domain, and interrupts blocked by a remapping table. */
unsigned long iommu_dma_faults(void);
unsigned long iommu_irq_faults(void);

/** Read the fault records a unit's last pass left full, when it reached
 * its bound: the boot processor's tick, with interrupts off. */
void iommu_tick(void);

/** Bring the units up: called once at boot, once ACPICA has the tables. */
void iommu_boot(void);

#endif /* _IOMMU_H_ */
