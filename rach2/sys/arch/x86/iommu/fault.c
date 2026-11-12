/*
 * fault.c: the VT-d units' fault reports: each unit's fault recording
 * registers read, counted and logged as `vtd: fault: ...`, from the
 * interrupt the unit raises for a new fault.
 *
 * A unit records a blocked DMA request or interrupt in one of its fault
 * recording registers (VT-d specification, 7.2 and 11.4.14): 16 bytes each,
 * the faulting page or interrupt index in the low quadword and, in the high
 * one, the F bit that marks it full, the access type, the reason and the
 * requester id. The fault status register gives the first full record; the
 * handler reads records from it, round the ring, until one is empty,
 * clearing each by writing its F bit as one, as Linux's dmar_fault does
 * (drivers/iommu/intel/dmar.c:1958-2035 in 6.12.115), so that a fault
 * recorded while the handler runs is read in the same pass, and it drops
 * the unit's lock between records, as dmar_fault does (:2015, :2026). A
 * fault that finds every record full is lost, and the unit says so only by
 * PFO, once however many were lost.
 *
 * Unlike dmar_fault, a pass reads at most FAULT_PASS_RECORDS records. A
 * device that keeps faulting refills a unit's records as fast as they are
 * read, the Lenovo's units having one each, and the pass would not end
 * until the device stops; so a pass that reaches the bound leaves the rest
 * to the next tick (`iommu_tick`), since a record left full keeps PPF set
 * and the unit raises no new fault event for it.
 */

#include "vtd.h"

#include "../../../iommu.h"
#include "../../../klog.h"

/* A unit's faults logged before the rest are only counted, and its passes
 * that found faults lost logged before the rest are only counted. */
#define FAULT_LOG_LIMIT 32u

/* The records one pass of a unit reads before it leaves the rest to the
 * next tick. */
#define FAULT_PASS_RECORDS 32u

/* Why a DMA request was blocked, by reason 0x00 to 0x0D, as Linux words it. */
static const char *const dma_reasons[] = {
    "Software",
    "Present bit in root entry is clear",
    "Present bit in context entry is clear",
    "Invalid context entry",
    "Access beyond MGAW",
    "PTE Write access is not set",
    "PTE Read access is not set",
    "Next page table ptr is invalid",
    "Root table address invalid",
    "Context table ptr is invalid",
    "non-zero reserved fields in RTP",
    "non-zero reserved fields in CTP",
    "non-zero reserved fields in PTE",
    "PCE for translation request specifies blocking",
};

/* Why an interrupt was blocked, by reason 0x20 to 0x26. */
static const char *const ir_reasons[] = {
    "Detected reserved fields in the decoded interrupt-remapped request",
    "Interrupt index exceeded the interrupt-remapping table size",
    "Present field in the IRTE entry is clear",
    "Error accessing interrupt-remapping table pointed by IRTA_REG",
    "Detected reserved fields in the IRTE entry",
    "Blocked a compatibility format interrupt request",
    "Blocked an interrupt request due to source-id verification failure",
};

/** The words for fault reason `reason`, and in `*interrupt` whether it is
 * an interrupt's. */
const char *vtd_fault_reason(unsigned int reason, int *interrupt)
{
    *interrupt = 0;
    if (reason >= 0x20u && reason - 0x20u < sizeof(ir_reasons) / sizeof(ir_reasons[0])) {
        *interrupt = 1;
        return ir_reasons[reason - 0x20u];
    }
    if (reason < sizeof(dma_reasons) / sizeof(dma_reasons[0])) {
        return dma_reasons[reason];
    }
    return "Unknown";
}

/**
 * Count the fault of record `hi` of `u`, under `u->lock`: whether to log
 * it, 1, to log that further faults go unlogged, 2, or neither, 0.
 */
static int fault_count(struct vtd_unit *u, uint64_t hi)
{
    int interrupt;

    (void)vtd_fault_reason((unsigned int)(hi >> 32) & 0xFFu, &interrupt);
    if (interrupt) {
        u->ir_faults++;
    } else {
        u->dma_faults++;
    }
    if (u->logged > FAULT_LOG_LIMIT) {
        return 0;
    }
    return u->logged++ == FAULT_LOG_LIMIT ? 2 : 1;
}

/** Log the fault of record (lo, hi) of `u` as `fault_count` said, `what`,
 * without the unit's lock. */
static void fault_log(const struct vtd_unit *u, uint64_t lo, uint64_t hi, int what)
{
    unsigned int reason = (unsigned int)(hi >> 32) & 0xFFu;
    unsigned int sid = (unsigned int)hi & 0xFFFFu;
    int read = (int)((hi >> 62) & 1u), interrupt;
    const char *why = vtd_fault_reason(reason, &interrupt);

    if (what == 0) {
        return;
    }
    if (what == 2) {
        klog(KLOG_ERR, "vtd", "fault: dmar%d: further faults counted, not logged", u->index);
        return;
    }
    if (interrupt) {
        klog(KLOG_ERR, "vtd", "fault: dmar%d: interrupt from %x:%x.%u, index 0x%x: reason 0x%x, %s",
             u->index, sid >> 8, (sid >> 3) & 0x1Fu, sid & 7u, (unsigned int)(lo >> 48), reason,
             why);
    } else {
        klog(KLOG_ERR, "vtd", "fault: dmar%d: dma %s from %x:%x.%u at 0x%lx: reason 0x%x, %s",
             u->index, read ? "read" : "write", sid >> 8, (sid >> 3) & 0x1Fu, sid & 7u,
             (unsigned long)(lo & ~0xFFFull), reason, why);
    }
}

/**
 * Read and clear the faults `u` has recorded, while records stay full, up
 * to FAULT_PASS_RECORDS of them, and the status bits that say so. A pass
 * that reaches the bound sets `u->fault_again`, and `iommu_tick` reads the
 * rest. A lost fault (PFO) counts as one DMA fault, since the unit does
 * not say how many it lost, and the log says the count is then a floor, for
 * the first FAULT_LOG_LIMIT passes that find one; a device that keeps
 * faulting sets PFO on nearly every pass, which then runs on every tick, so
 * the rest are only counted, as Linux rate-limits dmar_fault's line of the
 * status, which shows PFO, and of each record (drivers/iommu/intel/dmar.c:
 * 1964-1971, :1980 in 6.12.115).
 * An error of a device's invalidation is counted and cleared; a refused
 * descriptor of the queue is left to the queue's submitter, which waits on
 * it. Takes `u->lock`, and drops it to log each record, as Linux's
 * dmar_fault does; may run in the fault event's interrupt.
 */
void vtd_fault_unit(struct vtd_unit *u)
{
    uint32_t flags, fsts;
    unsigned int index, read = 0, fro = VTD_CAP_FRO(u->cap), nfr = VTD_CAP_NFR(u->cap);

    if (u->regs == 0) {
        return;
    }
    flags = spin_lock_irqsave(&u->lock);
    u->fault_again = 0;
    fsts = vtd_hw_read32(u->regs, VTD_FSTS);
    if ((fsts & (VTD_FSTS_ICE | VTD_FSTS_ITE)) != 0) {
        klog(KLOG_ERR, "vtd", "fault: dmar%d: device invalidation error, status 0x%x", u->index,
             fsts);
        u->errors++;
        vtd_hw_write32(u->regs, VTD_FSTS, fsts & (VTD_FSTS_ICE | VTD_FSTS_ITE));
    }
    if ((fsts & VTD_FSTS_PPF) != 0) {
        index = ((fsts >> 8) & 0xFFu) % nfr;
        for (;;) {
            unsigned int at = fro + index * 16u;
            uint64_t hi = vtd_hw_read64(u->regs, at + 8u), lo;
            int what;

            /* All ones: the unit's registers no longer answer. */
            if ((hi >> 63) == 0 || hi == ~0ull) {
                break;
            }
            if (read == FAULT_PASS_RECORDS) {
                u->fault_again = 1;
                break;
            }
            lo = vtd_hw_read64(u->regs, at);
            vtd_hw_write32(u->regs, at + 12u, 0x80000000u);
            what = fault_count(u, hi);
            read++;
            spin_unlock_irqrestore(&u->lock, flags);
            fault_log(u, lo, hi, what);
            index = (index + 1u) % nfr;
            flags = spin_lock_irqsave(&u->lock);
        }
    }
    /* Read again: PFO may have been set while the records were read. */
    fsts = vtd_hw_read32(u->regs, VTD_FSTS);
    if ((fsts & VTD_FSTS_PFO) != 0) {
        u->overflows++;
        u->dma_faults++;
        if (u->overflows <= FAULT_LOG_LIMIT) {
            klog(KLOG_ERR, "vtd",
                 "fault: dmar%d: faults lost, every record full: %lu times, so %lu DMA faults is "
                 "a floor",
                 u->index, u->overflows, u->dma_faults);
        }
        if (u->overflows == FAULT_LOG_LIMIT) {
            klog(KLOG_ERR, "vtd", "fault: dmar%d: further losses counted, not logged", u->index);
        }
    }
    vtd_hw_write32(u->regs, VTD_FSTS, (fsts & VTD_FSTS_PFO) | VTD_FSTS_PPF | VTD_FSTS_PRO);
    spin_unlock_irqrestore(&u->lock, flags);
}

/** Read every unit's faults: the fault event's handler. */
void vtd_fault_all(void)
{
    int i;

    for (i = 0; i < vtd_nunits; i++) {
        vtd_fault_unit(&vtd_units[i]);
    }
}

void iommu_tick(void)
{
    int i;

    for (i = 0; i < vtd_nunits; i++) {
        if (vtd_units[i].fault_again) {
            vtd_fault_unit(&vtd_units[i]);
        }
    }
}

/**
 * Point every unit's fault event at one message vector of this processor,
 * after reading the faults the firmware's time left: 0, or -1 when no
 * vector was free, in which case faults are read only when a unit's
 * faults are read for another reason.
 */
int vtd_fault_start(void)
{
    int i, vector;

    vtd_fault_all();
    vector = vtd_hw_fault_vector();
    if (vector < 0) {
        klog(KLOG_WARN, "vtd", "fault: no vector: faults go unreported");
        return -1;
    }
    for (i = 0; i < vtd_nunits; i++) {
        struct vtd_unit *u = &vtd_units[i];

        vtd_hw_write32(u->regs, VTD_FECTL, VTD_FECTL_IM);
        vtd_hw_write32(u->regs, VTD_FEDATA, (uint32_t)vector);
        vtd_hw_write32(u->regs, VTD_FEADDR, 0xFEE00000u | (vtd_hw_apic_id() << 12));
        vtd_hw_write32(u->regs, VTD_FEUADDR, 0);
        vtd_hw_write32(u->regs, VTD_FECTL, 0);
    }
    vtd_fault_all();
    return 0;
}
