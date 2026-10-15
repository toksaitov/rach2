/*
 * ioapic.c: the I/O APICs, as ioapic.h describes.
 *
 * An I/O APIC has two registers in memory: a select register at its base,
 * which selects one of its internal registers, and a window at base + 0x10,
 * through which that register is read and written. The redirection table has
 * an entry of two such registers for each input: the low one has the
 * vector, the delivery mode, the polarity, the trigger and the mask, and the
 * high one the destination's APIC identity in its top byte (82093AA
 * datasheet, 3.2.4).
 *
 * The select register is shared by every access, so each access takes the
 * pair with interrupts off, as configuration mechanism #1 does.
 */

#include "ioapic.h"

#include "../../klog.h"
#include "../../lib/kirq.h"
#include "../../mm/paging.h"

#define IOAPIC_SELECT 0
#define IOAPIC_WINDOW 4 /* in 32-bit words: byte offset 0x10 */

#define IOAPIC_REG_VERSION 0x01
#define IOAPIC_REG_TABLE   0x10

#define RTE_LOW_ACTIVE (1u << 13)
#define RTE_PENDING    (1u << 12)
#define RTE_LEVEL      (1u << 15)
#define RTE_MASKED     (1u << 16)

/* One I/O APIC the MADT lists and `ioapic_init` made ready. */
struct ioapic {
    volatile uint32_t *regs; /* its registers, mapped, by 32-bit word */
    uint32_t gsi_base;       /* the global system interrupt of input 0 */
    uint32_t inputs;         /* the entries of its redirection table */
    uint8_t id;
};

static struct ioapic chips[ACPI_MAX_IOAPICS];
static int nchips;

/* Where each ISA line was routed: its chip, and the input on it; chip -1
 * for a line not routed. */
static struct {
    int chip;
    uint32_t input;
} lines[16];

/** Read the internal register `reg` of `c` through its select register and
 * window. */
static uint32_t reg_read(const struct ioapic *c, uint32_t reg)
{
    uint32_t flags = kirq_save();
    uint32_t v;

    c->regs[IOAPIC_SELECT] = reg;
    v = c->regs[IOAPIC_WINDOW];
    kirq_restore(flags);
    return v;
}

/** Write `value` to the internal register `reg` of `c`. */
static void reg_write(const struct ioapic *c, uint32_t reg, uint32_t value)
{
    uint32_t flags = kirq_save();

    c->regs[IOAPIC_SELECT] = reg;
    c->regs[IOAPIC_WINDOW] = value;
    kirq_restore(flags);
}

int ioapic_init(const struct acpi_info *a)
{
    int i;
    uint32_t n;

    nchips = 0;
    for (i = 0; i < 16; i++) {
        lines[i].chip = -1;
    }
    for (i = 0; i < a->nioapics; i++) {
        struct ioapic *c = &chips[nchips];
        uintptr_t va = map_mmio((uintptr_t)a->ioapics[i].address, 0x20);
        uint32_t version;

        if (va == 0) {
            klog(KLOG_WARN, "ioapic", "refused: id %d at 0x%x could not be mapped",
                 (int)a->ioapics[i].id, a->ioapics[i].address);
            continue;
        }
        c->regs = (volatile uint32_t *)va;
        c->gsi_base = a->ioapics[i].gsi_base;
        c->id = a->ioapics[i].id;
        version = reg_read(c, IOAPIC_REG_VERSION);
        if (version == 0xFFFFFFFFu) {
            klog(KLOG_WARN, "ioapic", "refused: id %d at 0x%x does not answer", (int)c->id,
                 a->ioapics[i].address);
            continue;
        }
        /* Bits 16 to 23: the index of the last redirection entry. */
        c->inputs = ((version >> 16) & 0xFFu) + 1u;
        for (n = 0; n < c->inputs; n++) {
            reg_write(c, IOAPIC_REG_TABLE + 2u * n, RTE_MASKED);
        }
        nchips++;
        klog(KLOG_INFO, "ioapic", "ready: id %d at 0x%x, version 0x%x, gsi %d to %d", (int)c->id,
             a->ioapics[i].address, version & 0xFFu, (int)c->gsi_base,
             (int)(c->gsi_base + c->inputs - 1u));
    }
    return nchips;
}

int ioapic_route_line(int line, int pci, uint8_t vector, uint8_t dest)
{
    struct acpi_route r;
    uint32_t low;
    int i;

    if (line < 0 || line > 15 || acpi_isa_route(acpi_get(), line, pci, &r) != 0) {
        return -1;
    }
    for (i = 0; i < nchips; i++) {
        struct ioapic *c = &chips[i];

        if (r.gsi < c->gsi_base || r.gsi - c->gsi_base >= c->inputs) {
            continue;
        }
        low = vector | (r.low ? RTE_LOW_ACTIVE : 0u) | (r.level ? RTE_LEVEL : 0u);
        /* The destination first, while the entry is still masked. */
        reg_write(c, IOAPIC_REG_TABLE + 2u * (r.gsi - c->gsi_base) + 1u, (uint32_t)dest << 24);
        reg_write(c, IOAPIC_REG_TABLE + 2u * (r.gsi - c->gsi_base), low);
        lines[line].chip = i;
        lines[line].input = r.gsi - c->gsi_base;
        klog(KLOG_DEBUG, "ioapic", "route: line %d to gsi %d, vector %d, %s, active %s", line,
             (int)r.gsi, (int)vector, r.level ? "level" : "edge", r.low ? "low" : "high");
        return 0;
    }
    klog(KLOG_WARN, "ioapic", "route: no ioapic has gsi %d for line %d", (int)r.gsi, line);
    return -1;
}

void ioapic_mask_line(int line)
{
    const struct ioapic *c;
    uint32_t reg;

    if (line < 0 || line > 15 || lines[line].chip < 0) {
        return;
    }
    c = &chips[lines[line].chip];
    reg = IOAPIC_REG_TABLE + 2u * lines[line].input;
    reg_write(c, reg, reg_read(c, reg) | RTE_MASKED);
}

int ioapic_line_pending(int line)
{
    if (line < 0 || line > 15 || lines[line].chip < 0) {
        return 0;
    }
    return (reg_read(&chips[lines[line].chip], IOAPIC_REG_TABLE + 2u * lines[line].input) &
            RTE_PENDING) != 0;
}
