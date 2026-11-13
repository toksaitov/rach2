/*
 * vtd.h: the Intel VT-d driver's own declarations, which dmar.c, vtd.c,
 * intremap.c, fault.c and hw.c share; the rest of the kernel uses iommu.h.
 */

#ifndef _VTD_H_
#define _VTD_H_

#include "../../../lib/kint.h"
#include "../../../lib/spinlock.h"

#define VTD_MAX_UNITS       8
#define VTD_MAX_SCOPES      32 /* device scopes of one unit */
#define VTD_MAX_RMRRS       8
#define VTD_MAX_RMRR_SCOPES 8

/* A unit's registers (VT-d specification, 11.4). */
#define VTD_VER     0x00
#define VTD_CAP     0x08
#define VTD_ECAP    0x10
#define VTD_GCMD    0x18
#define VTD_GSTS    0x1C
#define VTD_RTADDR  0x20
#define VTD_CCMD    0x28
#define VTD_FSTS    0x34
#define VTD_FECTL   0x38
#define VTD_FEDATA  0x3C
#define VTD_FEADDR  0x40
#define VTD_FEUADDR 0x44
#define VTD_PMEN    0x64
#define VTD_IQH     0x80
#define VTD_IQT     0x88
#define VTD_IQA     0x90
#define VTD_ICS     0x9C
#define VTD_IRTA    0xB8

/* The global command and status bits, at the same places in both. */
#define VTD_GCMD_TE    (1u << 31) /* translation */
#define VTD_GCMD_SRTP  (1u << 30) /* set the root table pointer */
#define VTD_GCMD_WBF   (1u << 27) /* flush the write buffer */
#define VTD_GCMD_QIE   (1u << 26) /* queued invalidation */
#define VTD_GCMD_IRE   (1u << 25) /* interrupt remapping */
#define VTD_GCMD_SIRTP (1u << 24) /* set the remapping table pointer */
#define VTD_GCMD_CFI   (1u << 23) /* let compatibility-format interrupts by */

/* The fault status bits; each is cleared by writing it as one. */
#define VTD_FSTS_PFO (1u << 0) /* a fault was lost: every record was full */
#define VTD_FSTS_PPF (1u << 1) /* a fault record holds a fault */
#define VTD_FSTS_IQE (1u << 4) /* the invalidation queue met a bad descriptor */
#define VTD_FSTS_ICE (1u << 5) /* a device's invalidation completion failed */
#define VTD_FSTS_ITE (1u << 6) /* a device's invalidation timed out */
#define VTD_FSTS_PRO (1u << 7) /* a page request was lost */

#define VTD_FECTL_IM (1u << 31) /* the fault event's interrupt is masked */

/* The capability register's fields. */
#define VTD_CAP_ND(c)    ((unsigned int)((c) & 7u))
#define VTD_CAP_RWBF(c)  ((unsigned int)(((c) >> 4) & 1u))
#define VTD_CAP_CM(c)    ((unsigned int)(((c) >> 7) & 1u))
#define VTD_CAP_SAGAW(c) ((unsigned int)(((c) >> 8) & 0x1Fu))
#define VTD_CAP_MGAW(c)  ((unsigned int)((((c) >> 16) & 0x3Fu) + 1u))
#define VTD_CAP_FRO(c)   ((unsigned int)((((c) >> 24) & 0x3FFu) * 16u))
#define VTD_CAP_SLLPS(c) ((unsigned int)(((c) >> 34) & 0xFu))
#define VTD_CAP_NFR(c)   ((unsigned int)((((c) >> 40) & 0xFFu) + 1u))
#define VTD_CAP_DWD(c)   ((unsigned int)(((c) >> 54) & 1u))
#define VTD_CAP_DRD(c)   ((unsigned int)(((c) >> 55) & 1u))

/* The extended capability register's fields. */
#define VTD_ECAP_C(e)   ((unsigned int)((e) & 1u))        /* coherent walks */
#define VTD_ECAP_QI(e)  ((unsigned int)(((e) >> 1) & 1u)) /* queued invalidation */
#define VTD_ECAP_IR(e)  ((unsigned int)(((e) >> 3) & 1u)) /* interrupt remapping */
#define VTD_ECAP_EIM(e) ((unsigned int)(((e) >> 4) & 1u)) /* x2APIC ids in entries */
#define VTD_ECAP_PT(e)  ((unsigned int)(((e) >> 6) & 1u)) /* pass-through */
#define VTD_ECAP_SC(e)  ((unsigned int)(((e) >> 7) & 1u)) /* snoop control */
#define VTD_ECAP_IRO(e) ((unsigned int)((((e) >> 8) & 0x3FFu) * 16u))

/* A second-level paging entry's bits. */
#define VTD_PTE_R    1ull
#define VTD_PTE_W    2ull
#define VTD_PTE_PS   (1ull << 7) /* a large page */
#define VTD_PTE_SNP  (1ull << 11)
#define VTD_PTE_ADDR 0x000FFFFFFFFFF000ull

/* The context entry's translation types (low word, bits 3:2). */
#define VTD_TT_TRANSLATE   0u
#define VTD_TT_PASSTHROUGH 2u

/* The domain ids: 0 is reserved, as caching mode tags non-present entries
 * with it; 1 is the pass-through or identity domain of every device not
 * given to a VM; the domains of `iommu_domain_alloc` take the rest. */
#define VTD_DID_HOST  1u
#define VTD_DID_FIRST 2u
#define VTD_DID_MAX   256u

#define VTD_IRT_ENTRIES 256u /* each unit's remapping table: one page */

/* A device scope of the DMAR table, its path walked to the device. */
struct dmar_scope {
    uint8_t type;    /* 1 an endpoint, 2 a bridge and what is below it, 3 an
                        I/O APIC, 4 an HPET */
    uint8_t enum_id; /* the I/O APIC's or the HPET's id */
    uint8_t bus;     /* where the path ends */
    uint8_t devfn;
    uint8_t sec; /* for a bridge, the buses below it, sec to sub; a
                    bridge with no buses has sec 1 and sub 0 */
    uint8_t sub;
};

/* One remapping unit of the table, a DRHD. */
struct dmar_unit_desc {
    uint64_t base;
    uint16_t segment;
    uint8_t flags; /* bit 0: every PCI device of the segment not in another
                      unit's scope is in this one's */
    int nscopes;
    struct dmar_scope scopes[VTD_MAX_SCOPES];
};

/* One reserved region, an RMRR: memory the firmware has the devices of
 * its scope reach by DMA. `end` is the region's last byte. */
struct dmar_rmrr {
    uint64_t base;
    uint64_t end;
    uint16_t segment;
    int nscopes;
    struct dmar_scope scopes[VTD_MAX_RMRR_SCOPES];
};

/* What the DMAR table says. */
struct dmar_info {
    uint8_t haw;   /* the host address width, in bits */
    uint8_t flags; /* bit 0 interrupt remapping, bit 1 x2APIC opt-out, bit 2
                      DMA control opt-in */
    int nunits;
    struct dmar_unit_desc units[VTD_MAX_UNITS];
    int nrmrrs;
    struct dmar_rmrr rmrrs[VTD_MAX_RMRRS];
};

#define DMAR_INCLUDE_ALL 0x01u
#define DMAR_INTR_REMAP  0x01u
#define DMAR_X2APIC_OUT  0x02u /* the firmware asks that x2APIC mode not be used */

/* A remapping unit at work. */
struct vtd_unit {
    int index;                         /* dmarN */
    const struct dmar_unit_desc *desc; /* its row of the table */
    uintptr_t regs;                    /* its registers, mapped */
    uint64_t cap;
    uint64_t ecap;
    uint32_t gcmd;         /* the persistent command bits set */
    int levels;            /* its domains' table levels, 3 or 4 */
    unsigned int mgaw;     /* the widest input address, in bits */
    unsigned int ndomains; /* domain ids it has, capped at 256 */
    uint8_t did_used[VTD_DID_MAX / 8];
    uint64_t *root; /* the root table: a page */
    uint64_t root_phys;
    uint64_t *ctx[256]; /* each bus's context table, or NULL */
    uint64_t *iq;       /* the invalidation queue: a page */
    uint64_t iq_phys;
    unsigned int iq_tail;         /* the next descriptor's index */
    volatile uint32_t *iq_status; /* where wait descriptors write */
    uint64_t iq_status_phys;
    uint32_t iq_seq; /* the last wait's status value */
    uint64_t *irt;   /* the remapping table, or NULL */
    uint64_t irt_phys;
    uint8_t irte_used[VTD_IRT_ENTRIES / 8];
    struct iommu_domain *identity; /* the host's domain where the unit
                                      has no pass-through, or NULL */
    unsigned int assigned;         /* devices given to a VM's domain */
    int translating;               /* TE is set */
    int host_built;                /* the host's context entries exist */
    unsigned long dma_faults;      /* records read, plus one for each overflow */
    unsigned long ir_faults;
    unsigned long overflows; /* passes that found PFO set: faults lost */
    unsigned long errors;    /* invalidation errors */
    int fault_again;         /* a pass left full records for the next tick */
    int cfi_unreported;      /* GSTS.CFIS did not follow a set of GCMD.CFI */
    struct spinlock lock;    /* the registers and the tables */
};

/* A domain: one address space of second-level tables. */
struct iommu_domain {
    uint64_t *top; /* the top table */
    uint64_t top_phys;
    int levels;                        /* 3 or 4 */
    uint64_t limit;                    /* IOVAs below it, as the units allow */
    uint64_t mapped_end;               /* the end of the highest mapping */
    int snoop;                         /* set SNP in its leaves */
    int flush;                         /* a unit it is on walks incoherently */
    int identity;                      /* a unit's host identity domain */
    unsigned long pages;               /* 4 KiB pages mapped */
    uint16_t did[VTD_MAX_UNITS];       /* its id on each unit, 0 for none */
    unsigned int users[VTD_MAX_UNITS]; /* its devices on each unit */
};

/* dmar.c */
int dmar_parse(const uint8_t *table, uint32_t len, struct dmar_info *out);
int dmar_unit_for(const struct dmar_info *d, uint8_t bus, uint8_t devfn);
int dmar_rmrr_names(const struct dmar_rmrr *r, uint8_t bus, uint8_t devfn);
int dmar_ioapic_source(const struct dmar_info *d, uint8_t id, int *unit, uint16_t *sid);
void dmar_report(const struct dmar_info *d);

/* vtd.c */
extern struct dmar_info vtd_dmar;
extern struct vtd_unit vtd_units[VTD_MAX_UNITS];
extern int vtd_nunits;
int vtd_start(void);
void vtd_write_table(const struct vtd_unit *u, uint64_t *slot, uint64_t value);
void vtd_flush_entry(const struct vtd_unit *u, const void *p, unsigned long len);
int vtd_inv_iec(struct vtd_unit *u, int index);
void vtd_set_gcmd(struct vtd_unit *u, uint32_t bit, int on);
void vtd_gcmd_once(struct vtd_unit *u, uint32_t bit);

/* intremap.c */
int intremap_start(void);
void intremap_stop(void);
int intremap_active(void);
int intremap_x2apic(void);

/* fault.c */
int vtd_fault_start(void);

/** Mask every unit's fault event (`hold` 1), or unmask it (`hold` 0),
 * once `vtd_fault_start` aimed it: across the late x2APIC leave
 * (`iommu_fault_hold`). */
void vtd_fault_hold(int hold);

/** Point the units' fault events again at the boot processor, by the id it
 * answers to after its APIC changed mode. */
void vtd_fault_retarget(void);
void vtd_fault_unit(struct vtd_unit *u);
void vtd_fault_all(void);
const char *vtd_fault_reason(unsigned int reason, int *interrupt);

/*
 * hw.c: what the driver needs of the machine. A host test gives its own
 * versions, over a model of the units.
 */

/** Map a unit's 4 KiB of registers at `base`: their address, or 0. */
uintptr_t vtd_hw_map(uint64_t base);
uint32_t vtd_hw_read32(uintptr_t regs, unsigned int off);
uint64_t vtd_hw_read64(uintptr_t regs, unsigned int off);
void vtd_hw_write32(uintptr_t regs, unsigned int off, uint32_t v);
void vtd_hw_write64(uintptr_t regs, unsigned int off, uint64_t v);

/** A zeroed page the units may read, with its physical address in
 * `*phys`; NULL when memory is short. */
void *vtd_hw_page(uint64_t *phys);
void vtd_hw_page_free(void *va);

/** The kernel's address of a page `vtd_hw_page` gave, by its physical
 * address. */
void *vtd_hw_virt(uint64_t phys);

/** Write the cache lines of [p, p + len) back to memory, for a unit that
 * does not snoop its walks. */
void vtd_hw_flush(const void *p, unsigned long len);

void vtd_hw_udelay(unsigned int us);

/** The class code of the device at (bus, devfn), its three bytes, or
 * 0xFFFFFFFF when there is none. */
uint32_t vtd_hw_class(uint8_t bus, uint8_t devfn);

/** For the bridge at (bus, devfn), its secondary and subordinate buses: 0,
 * or -1 when it is not a bridge. */
int vtd_hw_bridge(uint8_t bus, uint8_t devfn, uint8_t *sec, uint8_t *sub);

/** Whether bus `bus` has a device at slot 0 to 31. */
int vtd_hw_bus_present(uint8_t bus);

/** The end of RAM: the byte past the highest address the frame map has. */
uint64_t vtd_hw_ram_top(void);

/** The DMAR table and its length, or NULL. */
const uint8_t *vtd_hw_dmar(uint32_t *len);

/** Whether the boot line turns translation off: the last `intel_iommu=`
 * item that is `off` or `on`, by its prefix, is `off`, as Linux's
 * intel_iommu_setup reads every token in order (Linux 6.12.115,
 * drivers/iommu/intel/iommu.c, lines 240 to 285). */
int vtd_hw_dma_off(void);

/** Whether the boot line turns interrupt remapping off, by `nointremap` or
 * `intremap=off` (`ioapic_intremap_off`). */
int vtd_hw_intremap_off(void);

/** Whether the boot line says `intremap=no_x2apic_optout`
 * (`ioapic_no_x2apic_optout`). */
int vtd_hw_no_x2apic_optout(void);

/** Whether the boot line keeps the I/O APIC off, and with it interrupt
 * remapping (`ioapic_disabled`). */
int vtd_hw_ioapic_off(void);

/** The ids of the I/O APICs the MADT lists, up to `max`: their count. */
int vtd_hw_ioapics(uint8_t *ids, int max);

/** Take a message vector for the units' fault events, with `vtd_fault_all`
 * on it: the vector, or -1. */
int vtd_hw_fault_vector(void);

/** Milliseconds since the boot, by the tick, wrapping at 2^32: the clock
 * of the fault log's rate limit. */
uint32_t vtd_hw_ms(void);

/** The local APIC id the units' fault events go to. */
uint32_t vtd_hw_apic_id(void);

/** The boot processor's whole x2APIC id, which the x2APIC format's entries
 * name it by, whichever mode its APIC is in (`lapic_x2apic_id`). */
uint32_t vtd_hw_x2apic_id(void);

/** Whether the local APIC is in x2APIC mode or may be moved there
 * (`lapic_x2apic_possible`). */
int vtd_hw_x2apic_possible(void);

/** Move the boot processor's APIC to x2APIC mode: 1 when it moved, 0 when
 * it was there, -1 when it cannot (`lapic_x2apic_enable`). */
int vtd_hw_x2apic_enable(void);

/* What `vtd_hw_lines_mask` changed, for `vtd_hw_lines_restore`. */
struct vtd_lines {
    uint32_t flags;  /* the processor's interrupt flag, as kirq_save gave it */
    uint16_t pic;    /* the 8259 pair's masks (`pic_mask_all`) */
    uint16_t ioapic; /* the I/O APIC lines that were unmasked */
};

/** Turn the processor's interrupts off and mask the 8259 pair and every
 * routed I/O APIC line, as Linux's enable_IR_x2apic does around the move to
 * x2APIC mode, keeping what was there in `*held`. */
void vtd_hw_lines_mask(struct vtd_lines *held);

/** Undo `vtd_hw_lines_mask`. */
void vtd_hw_lines_restore(const struct vtd_lines *held);

#endif /* _VTD_H_ */
