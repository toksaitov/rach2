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
 * handler reads records from it until one is empty, clearing each by
 * writing its F bit as one, as Linux's dmar_fault does.
 */

#include "vtd.h"

#include "../../../klog.h"

/* A unit's faults logged before the rest are only counted. */
#define FAULT_LOG_LIMIT 32u

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

/** Count and log the fault of record (lo, hi) of `u`. */
static void fault_one(struct vtd_unit *u, uint64_t lo, uint64_t hi)
{
    unsigned int reason = (unsigned int)(hi >> 32) & 0xFFu;
    unsigned int sid = (unsigned int)hi & 0xFFFFu;
    int read = (int)((hi >> 62) & 1u), interrupt;
    const char *why = vtd_fault_reason(reason, &interrupt);

    if (interrupt) {
        u->ir_faults++;
    } else {
        u->dma_faults++;
    }
    if (u->logged > FAULT_LOG_LIMIT) {
        return;
    }
    if (u->logged++ == FAULT_LOG_LIMIT) {
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
 * Read and clear every fault `u` has recorded, and the status bits that
 * say so. An error of a device's invalidation is counted and cleared; a
 * refused descriptor of the queue is left to the queue's submitter, which
 * waits on it. Takes `u->lock`; may run in the fault event's interrupt.
 */
void vtd_fault_unit(struct vtd_unit *u)
{
    uint32_t flags, fsts;
    unsigned int index, fro = VTD_CAP_FRO(u->cap), nfr = VTD_CAP_NFR(u->cap), n;

    if (u->regs == 0) {
        return;
    }
    flags = spin_lock_irqsave(&u->lock);
    fsts = vtd_hw_read32(u->regs, VTD_FSTS);
    if ((fsts & (VTD_FSTS_ICE | VTD_FSTS_ITE)) != 0) {
        klog(KLOG_ERR, "vtd", "fault: dmar%d: device invalidation error, status 0x%x", u->index,
             fsts);
        u->errors++;
        vtd_hw_write32(u->regs, VTD_FSTS, fsts & (VTD_FSTS_ICE | VTD_FSTS_ITE));
    }
    if ((fsts & VTD_FSTS_PFO) != 0) {
        klog(KLOG_ERR, "vtd", "fault: dmar%d: faults lost, every record full", u->index);
        u->dma_faults++;
    }
    if ((fsts & VTD_FSTS_PPF) != 0) {
        index = (fsts >> 8) & 0xFFu;
        for (n = 0; n < nfr; n++) {
            unsigned int at = fro + (index % nfr) * 16u;
            uint64_t hi = vtd_hw_read64(u->regs, at + 8u), lo;

            if ((hi >> 63) == 0) {
                break;
            }
            lo = vtd_hw_read64(u->regs, at);
            vtd_hw_write32(u->regs, at + 12u, 0x80000000u);
            fault_one(u, lo, hi);
            index++;
        }
    }
    vtd_hw_write32(u->regs, VTD_FSTS, VTD_FSTS_PFO | VTD_FSTS_PPF | VTD_FSTS_PRO);
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
