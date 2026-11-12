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
 *
 * While an interrupt remapper is in place, an entry's high half names an
 * entry of the remapper's table in place of the destination: its index in
 * bits 63:49 and the format in bit 48 (VT-d specification, 5.1.5.1). The
 * low half is the same in both formats, since the delivery is fixed and
 * the destination physical, whose zero bits 10:8 and 11 the remapped
 * format asks for there.
 */

#include "ioapic.h"

#include "../../cmdline.h"
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

/* Where each ISA line was routed: its chip, the input on it, the local APIC
 * it goes to, and the remapper's entry or -1; chip -1 for a line not
 * routed. */
static struct {
    int chip;
    uint32_t input;
    uint8_t dest;
    int handle;
} lines[16];

/* The interrupt remapper in place, or NULL. */
static const struct ioapic_remap *remap;

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

/** `maxcpus=` counts as 0 when its value begins with decimal digits that
 * read 0, as `smp_start` reads them; a value that begins with no digit is
 * passed over, as Linux's get_option passes it over. */
int ioapic_disabled(void)
{
    char v[12];
    int i;

    if (kcmdline_has("noapic")) {
        return 1;
    }
    if (!kcmdline_value("maxcpus", v, sizeof(v)) || v[0] < '0' || v[0] > '9') {
        return 0;
    }
    for (i = 0; v[i] >= '0' && v[i] <= '9'; i++) {
        if (v[i] != '0') {
            return 0;
        }
    }
    return 1;
}

/** Only the x86-64 kernel builds VT-d (platform.c). */
int ioapic_remappable(void)
{
#if defined(__x86_64__)
    return !ioapic_disabled();
#else
    return 0;
#endif
}

int ioapic_init(const struct acpi_info *a)
{
    int i;
    uint32_t n;

    nchips = 0;
    for (i = 0; i < 16; i++) {
        lines[i].chip = -1;
        lines[i].handle = -1;
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
    uint32_t low, high;
    int i, handle = -1;

    if (line < 0 || line > 15 || acpi_isa_route(acpi_get(), line, pci, &r) != 0) {
        return -1;
    }
    for (i = 0; i < nchips; i++) {
        struct ioapic *c = &chips[i];

        if (r.gsi < c->gsi_base || r.gsi - c->gsi_base >= c->inputs) {
            continue;
        }
        low = vector | (r.low ? RTE_LOW_ACTIVE : 0u) | (r.level ? RTE_LEVEL : 0u);
        high = (uint32_t)dest << 24;
        if (remap != NULL && (handle = remap->map(c->id, low, high, &high)) < 0) {
            klog(KLOG_WARN, "ioapic", "route: line %d has no remapping entry", line);
            return -1;
        }
        /* The destination first, while the entry is still masked. */
        reg_write(c, IOAPIC_REG_TABLE + 2u * (r.gsi - c->gsi_base) + 1u, high);
        reg_write(c, IOAPIC_REG_TABLE + 2u * (r.gsi - c->gsi_base), low);
        if (lines[line].handle >= 0 && remap != NULL) {
            remap->unmap(lines[line].handle);
        }
        lines[line].chip = i;
        lines[line].input = r.gsi - c->gsi_base;
        lines[line].dest = dest;
        lines[line].handle = handle;
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

uint16_t ioapic_mask_routed(void)
{
    uint16_t was = 0;
    int line;

    for (line = 0; line < 16; line++) {
        const struct ioapic *c;
        uint32_t reg, low;

        if (lines[line].chip < 0) {
            continue;
        }
        c = &chips[lines[line].chip];
        reg = IOAPIC_REG_TABLE + 2u * lines[line].input;
        low = reg_read(c, reg);
        if ((low & RTE_MASKED) == 0) {
            was = (uint16_t)(was | (1u << line));
            reg_write(c, reg, low | RTE_MASKED);
        }
    }
    return was;
}

void ioapic_unmask_lines(uint16_t which)
{
    int line;

    for (line = 0; line < 16; line++) {
        const struct ioapic *c;
        uint32_t reg;

        if ((which & (1u << line)) == 0 || lines[line].chip < 0) {
            continue;
        }
        c = &chips[lines[line].chip];
        reg = IOAPIC_REG_TABLE + 2u * lines[line].input;
        reg_write(c, reg, reg_read(c, reg) & ~RTE_MASKED);
    }
}

int ioapic_remap_start(const struct ioapic_remap *ops)
{
    int line, back;

    for (line = 0; line < 16; line++) {
        const struct ioapic *c;
        uint32_t reg, high;

        if (lines[line].chip < 0) {
            continue;
        }
        c = &chips[lines[line].chip];
        reg = IOAPIC_REG_TABLE + 2u * lines[line].input;
        lines[line].handle = ops->map(c->id, reg_read(c, reg), reg_read(c, reg + 1u), &high);
        if (lines[line].handle < 0) {
            for (back = 0; back < line; back++) {
                if (lines[back].chip >= 0 && lines[back].handle >= 0) {
                    c = &chips[lines[back].chip];
                    reg_write(c, IOAPIC_REG_TABLE + 2u * lines[back].input + 1u,
                              (uint32_t)lines[back].dest << 24);
                    ops->unmap(lines[back].handle);
                    lines[back].handle = -1;
                }
            }
            return -1;
        }
        reg_write(c, reg + 1u, high);
    }
    remap = ops;
    return 0;
}

void ioapic_remap_stop(void)
{
    int line;

    for (line = 0; line < 16; line++) {
        if (lines[line].chip >= 0 && lines[line].handle >= 0 && remap != NULL) {
            reg_write(&chips[lines[line].chip], IOAPIC_REG_TABLE + 2u * lines[line].input + 1u,
                      (uint32_t)lines[line].dest << 24);
            remap->unmap(lines[line].handle);
            lines[line].handle = -1;
        }
    }
    remap = NULL;
}
