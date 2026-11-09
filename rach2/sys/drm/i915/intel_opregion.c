/*
 * intel_opregion.c: the ACPI OpRegion, the firmware's 8 KiB of mailboxes
 * for the graphics driver, found through ASLS and checked as Linux 6.12's
 * intel_opregion.c checks it, with the VBT it holds.
 */

#include "intel_bios.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"
#include "../../mm/paging.h"

/* The header (struct opregion_header): the signature, the size in KiB, the
 * version's revision, minor and major bytes, and the mailbox bits. */
#define OPREGION_SIGNATURE "IntelGraphicsMem"
#define OPREGION_HDR_SIZE  0x10u
#define OPREGION_HDR_REV   0x15u
#define OPREGION_HDR_MINOR 0x16u
#define OPREGION_HDR_MAJOR 0x17u
#define OPREGION_HDR_MBOX  0x58u

/* Mailbox 1 (struct opregion_acpi): CHPD, the hotplug enable indicator. */
#define ACPI_CHPD 0xa8u

/* Mailbox 3 (struct opregion_asle): ARDY, the driver's readiness for ASLE
 * requests; RVDA, the raw VBT's address, physical on version 2.0 and an
 * offset from the OpRegion from 2.1; RVDS, its size. */
#define ASLE_ARDY           0x00u
#define ASLE_ARDY_NOT_READY 0u
#define ASLE_RVDA           0xbau
#define ASLE_RVDS           0xc2u

/** The little-endian 32 bits at `p`. */
static uint32_t get32(const volatile uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/** Write `v` little-endian at `p`, a field the firmware's AML reads. */
static void put32(volatile uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/** Map `size` bytes of firmware memory at `phys` uncached; NULL when the
 * MMIO region has no room or the range passes what an entry holds. */
static uint8_t *opregion_map(uint64_t phys, uint32_t size)
{
    uintptr_t va = mmio_map(phys, size, PTE_NOCACHE);

    return (uint8_t *)va;
}

/**
 * Find a valid VBT at RVDA, which a version 2 or later OpRegion may name
 * in mailbox 3 in place of mailbox 4, as Linux does; 1 when `op` now holds
 * it in its window `rvda`, 0 to go on to mailbox 4.
 */
static int opregion_find_rvda(struct intel_opregion *op)
{
    uint64_t rvda;
    uint32_t rvds;

    if (op->major < 2 || !op->asle) {
        return 0;
    }
    rvda = (uint64_t)get32(op->asle + ASLE_RVDA) | (uint64_t)get32(op->asle + ASLE_RVDA + 4) << 32;
    rvds = get32(op->asle + ASLE_RVDS);
    if (rvda == 0 || rvds == 0) {
        return 0;
    }
    /* From 2.1 RVDA is an offset from the OpRegion, past its end. */
    if (op->major > 2 || op->minor >= 1) {
        if (rvda < OPREGION_SIZE) {
            klog(KLOG_WARN, "i915", "opregion: RVDA 0x%lx lies inside the OpRegion",
                 (unsigned long)rvda);
        }
        rvda += op->phys;
    }
    op->rvda = opregion_map(rvda, rvds);
    if (!op->rvda) {
        return 0;
    }
    if (intel_bios_is_valid_vbt(op->rvda, rvds)) {
        op->rvda_size = rvds;
        op->vbt = op->rvda;
        op->vbt_size = rvds;
        return 1;
    }
    klog(KLOG_INFO, "i915", "opregion: invalid VBT at RVDA 0x%lx", (unsigned long)rvda);
    mmio_unmap((uintptr_t)op->rvda, rvds);
    op->rvda = NULL;
    return 0;
}

int32_t intel_opregion_setup(struct intel_opregion *op, uint32_t asls)
{
    uint8_t *base;
    uint32_t size;
    int i;

    k_memset(op, 0, sizeof(*op));
    if (asls == 0) {
        return -ENODEV;
    }
    base = opregion_map(asls, OPREGION_SIZE);
    if (!base) {
        return -ENOMEM;
    }
    for (i = 0; i < 16; i++) {
        if (base[i] != (uint8_t)OPREGION_SIGNATURE[i]) {
            klog(KLOG_WARN, "i915", "opregion: no signature at 0x%08x", (unsigned)asls);
            mmio_unmap((uintptr_t)base, OPREGION_SIZE);
            return -EINVAL;
        }
    }
    op->base = base;
    op->phys = asls;
    op->major = base[OPREGION_HDR_MAJOR];
    op->minor = base[OPREGION_HDR_MINOR];
    op->revision = base[OPREGION_HDR_REV];
    op->mboxes = get32(base + OPREGION_HDR_MBOX);

    if (op->mboxes & MBOX_ACPI) {
        op->acpi = base + OPREGION_ACPI_OFFSET;
        /* The driver handles monitor hotplug itself, so the firmware's
         * AML need not notify it; Windows too turns them off. */
        put32(op->acpi + ACPI_CHPD, 1);
    }
    if (op->mboxes & MBOX_SWSCI) {
        if (op->major >= 3) {
            klog(KLOG_WARN, "i915", "opregion: SWSCI mailbox on version %u, ignored",
                 (unsigned)op->major);
        } else {
            op->swsci = base + OPREGION_SWSCI_OFFSET;
        }
    }
    if (op->mboxes & MBOX_ASLE) {
        op->asle = base + OPREGION_ASLE_OFFSET;
        put32(op->asle + ASLE_ARDY, ASLE_ARDY_NOT_READY);
    }
    if (op->mboxes & MBOX_ASLE_EXT) {
        op->asle_ext = base + OPREGION_ASLE_EXT_OFFSET;
    }

    if (opregion_find_rvda(op)) {
        klog(KLOG_INFO, "i915",
             "opregion: version %u.%u.%u at 0x%08x, mailboxes 0x%02x; VBT at RVDA, %u bytes",
             (unsigned)op->major, (unsigned)op->minor, (unsigned)op->revision, (unsigned)asls,
             (unsigned)op->mboxes, (unsigned)op->vbt_size);
        return 0;
    }
    /* Mailbox 4 runs to mailbox 5, or to the end when there is none: some
     * boards' VBT runs into mailbox 5's place, against the specification,
     * and Linux allows it. */
    size = ((op->mboxes & MBOX_ASLE_EXT) ? OPREGION_ASLE_EXT_OFFSET : OPREGION_SIZE) -
           OPREGION_VBT_OFFSET;
    if (intel_bios_is_valid_vbt(base + OPREGION_VBT_OFFSET, size)) {
        op->vbt = base + OPREGION_VBT_OFFSET;
        op->vbt_size = size;
    }
    klog(KLOG_INFO, "i915", "opregion: version %u.%u.%u at 0x%08x, mailboxes 0x%02x; %s",
         (unsigned)op->major, (unsigned)op->minor, (unsigned)op->revision, (unsigned)asls,
         (unsigned)op->mboxes, op->vbt ? "VBT in mailbox 4" : "no valid VBT");
    return 0;
}

void intel_opregion_fini(struct intel_opregion *op)
{
    if (op->rvda) {
        mmio_unmap((uintptr_t)op->rvda, op->rvda_size);
    }
    if (op->base) {
        mmio_unmap((uintptr_t)op->base, OPREGION_SIZE);
    }
    k_memset(op, 0, sizeof(*op));
}
